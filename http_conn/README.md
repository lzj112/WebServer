Linux高性能服务器编程中十五章的Web服务器

封装了一个http_conn类,其中解析HTTP请求,封装了读取发送函数
epollfd为其静态成员
创建一个该类数组`users`
1. main中epoll监听,新连接添加到`users`,读写事件都调用的是http_conn封装的函数
2. 读完添加到任务队列等待线程池取,该代码epoll用的oneshot,每次要重新添加,解析完如果什么都没请求重新添加epoll读事件,否则添加epoll写事件
3. 等待epoll写事件触发,非阻塞发送数据