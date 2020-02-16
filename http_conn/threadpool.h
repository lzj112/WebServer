#ifndef THREADPOOL_H_
#define THREADPOOL_H_

#include <cstdio>
#include <exception>
#include <pthread.h>

#include <list>
#include <iostream>

#include "locker.h"
using namespace std;

//线程池类,一个模板类
template <typename T>
class threadpool
{
public:
	//thread_num是线程池中线程的数量,max_requests是请求队列中最多允许的等待处理的请求的数量
	threadpool(int pthread_num = 8, int max_requests = 1000);
	~threadpool();
	//往请求队列中添加任务
	bool append(T *request);

private:
	//工作线程运行的函数,不断从工作队列中取出任务并执行之
	static void *work(void *arg);
	void run();

private:
	//线程池中线程数
	int m_pthread_num;
	//请求队列中允许的最大请求数
	int m_max_requests;
	//描述线程池的数组,大小为m_thread_number
	pthread_t *pthreads;
	//请求队列
	list<T *> m_workqueue;
	//保护请求队列的互斥锁
	locker m_queuelocker;
	//是否有任务需要处理
	sem m_queuestat;
	//是否结束线程
	bool m_stop;
};

template <typename T>
threadpool<T>::threadpool(int pthread_num, int max_requests) 
	: m_pthread_num(pthread_num), 
	  m_max_requests(max_requests), 
	  pthreads(NULL), 
	  m_stop(false)
{
	if (pthread_num <= 0 || max_requests <= 0)
		throw std::exception();
	pthreads = new pthread_t[m_pthread_num];
	if (!pthreads)
		throw exception();
	
	//设置thread_number个线程,并将他们都设置为脱离线程
	for (int i = 0; i < m_pthread_num; i++)
	{
		cout << "create the " << i << "th pthreads" << endl;
		if (pthread_create(pthreads + i, NULL, work, this) != 0)
		{
			delete[] pthreads;
			throw exception();
		}
		if (pthread_detach(pthreads[i]))
		{
			delete[] pthreads;
			throw std::exception();
		}
	}
}

template <typename T>
threadpool<T>::~threadpool()
{
	pthreads = NULL;
	m_stop = true;
}

template <typename T>
bool threadpool<T>::append(T *request)
{
	//操作工作队列是一定要加锁,因为他被所有线程共享
	m_queuelocker.lock();
	if (m_workqueue.size() > m_max_requests)
	{
		m_queuelocker.unlock();
		return false;
	}
	m_workqueue.push_back(request);
	m_queuelocker.unlock();
	m_queuestat.post();
	return true;
}

template <typename T>
void *threadpool<T>::work(void *arg)
{
	threadpool *pool = (threadpool *)arg;
	pool->run();
	return pool;
}

template <typename T>
void threadpool<T>::run()
{
	while (!m_stop)
	{
		m_queuestat.wait();
		m_queuelocker.lock();
		if (m_workqueue.empty())
		{
			m_queuelocker.unlock();
			continue;
		}
		T *request = m_workqueue.front();
		m_workqueue.pop_front();
		m_queuelocker.unlock();
		if (!request)
			continue;
		request->process();
	}
}
#endif
