#include <signal.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000

extern int addfd(int epollfd, int fd, bool one_shot);
extern int removefd(int epollfd, int fd);

void addsig(int sig, void(handler)(int), bool restart = true)
{
	struct sigaction sa;
	memset(&sa, '\0', sizeof(sa));
	sa.sa_handler = handler;
	if (restart)
	{
		sa.sa_flags |= SA_RESTART;
	}
	sigfillset(&sa.sa_mask);
	assert(sigaction(sig, &sa, NULL) != -1);
}

void show_error(int connfd, const char *text)
{
	cout << text << endl;
	send(connfd, text, strlen(text), 0);
	close(connfd);
}

int main(int argc, char* arhv[])
{
	//if(argc<=2)
	//	 cout<<"usage: "<<argv[0]<<" ip address port_number";
	// const char *ip = "127.0.0.1";
	int port = 3000;

	//忽略SIGPIPE的信号
	// addsig(SIGPIPE, SIG_IGN);

	//创建线程池
	threadpool<http_conn> *pool = NULL;
	try
	{
		pool = new threadpool<http_conn>;
	}
	catch (...)
	{
		return 1;
	}

	//预先为每个可能的客户连接分配一个http_conn对象
	http_conn *user = new http_conn[MAX_FD];
	assert(user);
	int user_count = 0;

	int listenfd = socket(PF_INET, SOCK_STREAM, 0);
	assert(listenfd >= 0);
	struct linger tmp = {1, 0};
	setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));

	int ret = 0;
	struct sockaddr_in ser;
	bzero(&ser, sizeof(ser));
	ser.sin_family = AF_INET;
	ser.sin_addr.s_addr = htonl(INADDR_ANY); 
	// inet_pton(AF_INET, ip, &ser.sin_addr);
	ser.sin_port = htons(port);

	ret = bind(listenfd, (struct sockaddr *)&ser, sizeof(ser));
	assert(ret >= 0);

	ret = listen(listenfd, 5);
	assert(ret >= 0);

	epoll_event events[MAX_EVENT_NUMBER];
	int epollfd = epoll_create(5);
	assert(epollfd != -1);
	addfd(epollfd, listenfd, false);
	http_conn::m_epollfd = epollfd;

	while (true)
	{
		int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
		if (num < 0 && errno != EINTR)
		{
			cout << "epoll_wait fail" << endl;
			break;
		}
		for (int i = 0; i < num; i++)
		{
			int sockfd = events[i].data.fd;
			if (sockfd == listenfd)
			{
				struct sockaddr_in cli;
				socklen_t len = sizeof(cli);
				int connfd = accept(listenfd, (struct sockaddr *)&cli, &len);
				if (connfd < 0)
				{
					cout << "connect failed" << endl;
					continue;
				}
				if (http_conn::m_user_count >= MAX_FD)
				{
					show_error(connfd, "internet busy");
					continue;
				}
				//初始化客户连接,添加到用户数组
				user[connfd].init(connfd, cli);
			}
			else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
			{
				//如果有异常,直接关闭客户连接
				user[sockfd].close_conn();
			}
			else if (events[i].events & EPOLLIN)
			{
				//根据读的结果,决定是否将任务添加到线程池,还是关闭连接
				if (user[sockfd].read())
					pool->append(user + sockfd);//sockfd同时是下标,这里就是计算sockfd个偏移
				else
					user[sockfd].close_conn();
			}
			else if (events[i].events & EPOLLOUT)
			{
				//根据写的结果,决定是否关闭连接
				if (!user[sockfd].write())
					user[sockfd].close_conn();
			}
			else
			{}
		}
	}
	close(epollfd);
	close(listenfd);
	delete[] user;
	delete[] pool;
	return 0;
}
