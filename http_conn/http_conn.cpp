#include "http_conn.h"

//定义HTTP相应的一些状态信息
const char *ok_200_title = "ok";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has syntax or is inherently impossible to satisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "you do not have permission to get file from this server.\n";
const char *error_404_title = "not found";
const char *error_404_form = "the requested file was not found on this server.\n";
const char *error_500_title = "internal error";
const char *error_500_form = "there was an unuaual problem serving the request file.\n";
//网站的根目录
const char *doc_root = "var/www/html";

int setnonblocking(int fd)
{
	int old_option = fcntl(fd, F_GETFL);
	int new_option = old_option | O_NONBLOCK;
	fcntl(fd, F_SETFL, new_option);
	return old_option;
}

void addfd(int epollfd, int fd, bool enable)
{
	epoll_event event;
	event.data.fd = fd;
	event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
	if (enable)
		event.events |= EPOLLONESHOT;
	epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
	setnonblocking(fd);
}

void removefd(int epollfd, int fd)
{
	epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
	close(fd);
}

void modfd(int epollfd, int fd, int ev)
{
	epoll_event event;
	event.data.fd = fd;
	event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;

	epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn(bool real_close)
{
	if (real_close && (m_sockfd != -1))
	{
		removefd(m_epollfd, m_sockfd);
		m_sockfd = -1;
		m_user_count--; //关闭一个连接时,将客户总量减一
	}
}

void http_conn::init(int sockfd, const sockaddr_in &addr)
{
	m_sockfd = sockfd;
	m_address = addr;
	m_user_count++;

	//以下部分为了避免TIME_WAIT状态,仅为了调试,实际使用中应该去掉
	int reuse = 1;
	setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	addfd(m_epollfd, sockfd, true);
	//

	init();
}

void http_conn::init()
{
	m_check_state = CHECK_STATE_REQUESTLINE;
	m_linger = false;
	m_method = GET;
	m_url = 0;
	m_version = 0;
	m_content_length = 0;
	m_host = 0;
	m_check_index = 0;
	m_start_line = 0;
	m_read_index = 0;
	m_write_index = 0;
	memset(m_read_buf, '\0', READ_BUF_SIZE);
	memset(m_write_buf, '\0', WRITE_BUF_SIZE);
	memset(m_real_file, '\0', MAXFILENAME_LEN);
}

//从状态机
http_conn::LINE_STATUS http_conn::parse_line()
{
	char temp;
	for (; m_check_index < m_read_index; m_check_index++)
	{
		temp = m_read_buf[m_check_index];
		if (temp == '\r')
		{
			if ((m_check_index + 1) == m_read_index)
				return LINE_OPEN;
			else if (m_read_buf[m_check_index + 1] == '\n')
			{
				//将结尾符\r\n转换为两个\0
				m_read_buf[m_check_index++] = '\0';
				m_read_buf[m_check_index++] = '\0';
				return LINE_OK;
			}
			return LINE_BAD;
		}
		else if (temp == '\n')
		{
			if (m_check_index > 1 && (m_read_buf[m_check_index - 1] == '\r'))
			{
				m_read_buf[m_check_index - 1] == '\0';
				m_read_buf[m_check_index++] = '\0';
				return LINE_OK;
			}
			return LINE_BAD;
		}
	}
	return LINE_OPEN;
}

//循环读取客户数据,直到无数据可读或者对方关闭连接
bool http_conn::read()
{
	//读缓冲区已满
	if (m_read_index > READ_BUF_SIZE)
		return false;
	int bytes_read = 0;
	while (true)
	{
		//每次读取读缓冲区剩余字节数量
		bytes_read = recv(m_sockfd, m_read_buf + m_read_index, READ_BUF_SIZE - m_read_index, 0);
		if (bytes_read == -1)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			return false;
		}
		else if (bytes_read == 0)
			return false;
		m_read_index += bytes_read;
	}
	return true;
}

//解析HTTP请求行,获得请求方法,目标URL,以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
	m_url = strpbrk(text, "\t");
	if (!m_url)
		return BAD_REQUEST;
	*m_url++ = '\0';

	char *method = text;
	if (strcasecmp(method, "GET") == 0)
		m_method = GET;
	else
		return BAD_REQUEST;
	
	m_url += strspn(m_url, "\t");
	m_version += strspn(m_url, "\t");
	if (!m_version)
		return BAD_REQUEST;
	*m_version += '\0';
	m_version += strspn(m_version, "\t");
	if (strcasecmp(m_version, "HTTP/1.1") != 0)
		return BAD_REQUEST;
	if (strncasecmp(m_url, "http://", 7) == 0)
	{
		m_url += 7;
		m_url = strchr(m_url, '/');
	}
	if (!m_url || m_url[0] != '/')
		return BAD_REQUEST;
	
	//更新主状态机状态,变为检查请求头
	m_check_state = CHECK_STATE_HEADER;
	return NO_REQUEST;
}

//解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
	//遇到空行,表示头部字段解析完毕
	if (text[0] == '\0')
	{
		//如果HTTP请求有消息体,则还需要读取m_content_length字节的消息体,状态机转移到CHECK_STATE_CONTENT状态
		if (m_content_length != 0)
		{
			m_check_state = CHECK_STATE_CONTENT;
			return NO_REQUEST;
		}
		//否则说明我们已经得到了一个完整的HTTP请求
		return GET_REQUEST;
	}
	//处理Connection头部字段
	else if (strncasecmp(text, "Connection:", 11) == 0)
	{
		text += 11;
		text += strspn(text, "\t");
	
		if (strcasecmp(text, "keep-alive") == 0) 
			m_linger = true;
		// m_content_length = atol(text);
	}
	//处理Content-Length头部字段
	else if (strncasecmp(text, "Content-Length:", 15) == 0) 
	{
		text += 15;
		text += strspn(text, "\t");
		m_content_length = atol(text);
	}
	//处理HOST头部字段
	else if (strncasecmp(text, "Host:", 5) == 0)
	{
		text += 5;
		text += strspn(text, "\t");
		m_host = text;
	}
	else
		cout << "oop unknow host:" << text << endl;
	return NO_REQUEST;
}

//我们没有真正解析HTTP请求的消息体,只是判断他是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
	if (m_read_index >= (m_check_index + m_content_length))
	{
		text[m_content_length] = '\0';
		return GET_REQUEST;
	}
	return NO_REQUEST;
}

//主状态机
http_conn::HTTP_CODE http_conn::process_read()
{
	LINE_STATUS line_status = LINE_OK;
	HTTP_CODE ret = NO_REQUEST;
	char *text = 0;

	//调用从状态机(parse_line)
	while ((((m_check_state == CHECK_STATE_CONTENT) && 
			 (line_status == LINE_OK)) || 
			 ((line_status = parse_line()) == LINE_OK)))
	{
		//text指向读缓冲区该解析的位置
		text = get_line();
		//m_check_index在从状态机已经更新到行尾
		m_start_line == m_check_index;
		cout << "get 1 http line:" << text << endl;

		switch (m_check_state)
		{
			case CHECK_STATE_REQUESTLINE:
			{
				ret = parse_request_line(text);
				if (ret == BAD_REQUEST)
					return BAD_REQUEST;
				break;
			}
			case CHECK_STATE_HEADER:
			{
				ret = parse_headers(text);
				if (ret == BAD_REQUEST)
					return BAD_REQUEST;
				else if (ret == GET_REQUEST)
					return do_request();
				break;
			}
			case CHECK_STATE_CONTENT:
			{
				ret = parse_content(text);
				if (ret == GET_REQUEST)
					return do_request();
				line_status = LINE_OPEN;
				break;
			}
			default:
				return INTERNAL_ERROR;
		}
	}

	return NO_REQUEST;
}

//当得到一个完整的,正确HTPP请求时,我们就分析目标文件的属性,如果目标文件存在,对所有用户可读
//且不是目录,则使用mmap将其映射到内存地址m_file_address处,并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
	strcpy(m_real_file, doc_root);
	int len = strlen(doc_root);
	strncpy(m_real_file, m_url, MAXFILENAME_LEN - len - 1);
	if (stat(m_real_file, &m_file_stat) < 0)
		return NO_RESOURCE;

	if (!(m_file_stat.st_mode & S_IROTH))
		return FORBIDDEN_REQUEST;

	if (S_ISDIR(m_file_stat.st_mode))
		return BAD_REQUEST;

	int fd = open(m_real_file, O_RDONLY);
	m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	return FILE_REQUEST;
}

//对内存映射区域执行munmap操作
void http_conn::unmap()
{
	if (m_file_address)
	{
		munmap(m_file_address, m_file_stat.st_size);
		m_file_address = 0;
	}
}

//写HTTP相应
bool http_conn::write()
{
	int temp = 0;
	int bytes_have_send = 0;
	int bytes_to_send = m_write_index;
	if (bytes_to_send == 0)
	{
		modfd(m_epollfd, m_sockfd, EPOLLIN);
		init();
		return true;
	}

	while (1)
	{
		temp = writev(m_sockfd, m_iv, m_iv_count);
		if (temp <= -1)
		{
			//如果TCP写缓冲没有空间,则等待下一轮EPOLLOUT时间,虽然在此期间,
			//服务器无法立即接受到同一客户的下一个请求,但这可以保证连接的完整性
			if (errno == EAGAIN)
			{
				modfd(m_epollfd, m_sockfd, EPOLLOUT);
				return true;
			}
			unmap();
			return false;
		}

		bytes_to_send -= temp;
		bytes_have_send += temp;
		if (bytes_to_send < bytes_have_send)
		{
			//发送HTTP响应成功,根据HTTP请求中的Connection字段决定是否立即关闭连接
			unmap();
			if (m_linger)
			{
				init();
				modfd(m_epollfd, m_sockfd, EPOLLIN);
				return true;
			}
			else
			{
				modfd(m_epollfd, m_sockfd, EPOLLIN);
				return false;
			}
		}
	}
}

//往写缓冲区中写入待发送的数据
bool http_conn::add_response(const char *format, ...)
{
	if (m_write_index >= WRITE_BUF_SIZE)
		return false;
	va_list arg;
	va_start(arg, format);
	int len = vsnprintf(m_write_buf + m_write_index, WRITE_BUF_SIZE - 1 - m_write_index, format, arg);
	if (len >= (WRITE_BUF_SIZE - 1 - m_write_index))
		return false;
	m_write_index += len;
	va_end(arg);
	return true;
}

bool http_conn::add_status(int status, const char *title)
{
	return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len)
{
	add_content_length(content_len);
	add_linger();
	add_blank_line();
	return true;
}

bool http_conn::add_content_length(int content_len)
{
	return add_response("Content-length: %d\r\n", content_len);
}

bool http_conn::add_linger()
{
	return add_response("connection :%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line()
{
	return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char *content)
{
	return add_response("%s", content);
}

//根据服务器处理的HTTP请求的结果,界定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret)
{
	switch (ret)
	{
		case INTERNAL_ERROR:
		{
			add_status(500, error_500_title);
			add_headers(strlen(error_500_form));
			if (!add_content(error_500_form))
				return false;
			break;
		}
		case BAD_REQUEST:
		{
			add_status(400, error_400_title);
			add_headers(strlen(error_400_form));
			if (!(add_content(error_400_form)))
				return false;
			break;
		}
		case NO_RESOURCE:
		{
			add_status(404, error_404_title);
			add_headers(strlen(error_404_form));
			if (!add_content(error_404_form))
				return false;
			break;
		}
		case FORBIDDEN_REQUEST:
		{
			add_status(403, error_403_title);
			add_headers(strlen(error_403_form));
			if (!add_content(error_403_form))
				return false;
			break;
		}
		case FILE_REQUEST:
		{
			add_status(200, ok_200_title);
			if (m_file_stat.st_size != 0)
			{
				add_headers(m_file_stat.st_size);
				m_iv[0].iov_base = m_write_buf;
				m_iv[0].iov_len = m_write_index;
				m_iv[1].iov_base = m_file_address;
				m_iv[1].iov_len = m_file_stat.st_size;
				m_iv_count = 2;
				return true;
			}
			else
			{
				const char *okstring = "<html><body></body></html>";
				add_headers(strlen(okstring));
				if (!add_content(okstring))
					return false;
			}
		}
		default:
			return false;
	}
	m_iv[0].iov_base = m_write_buf;
	m_iv[0].iov_len = m_write_index;
	m_iv_count = 1;
	return true;
}

//由线程池中的工作线程调用,这是处理HTPP请求的入口函数
void http_conn::process()
{
cout << "here is process" << endl;
	//解析HTTP请求
	HTTP_CODE read_ret = process_read();
	if (read_ret == NO_REQUEST)
	{
		//什么都没有请求,重新添加到epoll读事件
		modfd(m_epollfd, m_sockfd, EPOLLIN);
		return;
	}
	bool write_ret = process_write(read_ret);
	if (!(write_ret))
		close_conn();
	modfd(m_epollfd, m_sockfd, EPOLLOUT);
}
