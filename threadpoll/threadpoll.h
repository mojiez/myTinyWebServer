#ifndef THREADPOLL_H
#define THREADPOLL_H

#include<iostream>
#include <pthread.h>
#include<list>
#include"../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template<typename T>

class threadpoll{
    public:
        // thread_number是线程池中线程的数量
        // max_requests是请求队列中最多允许的、等待处理的请求的数量
        threadpoll(connection_pool *connPool,int thread_number=8,int max_request=10000);
        ~threadpoll();

        // 向请求队列中插入任务请求
        bool append(T* request);
    
    private:
        // 工作线程运行的函数
        // 它不断从工作队列中取出任务并执行之
        // 线程函数指在一个独立的线程中执行的函数。在多线程编程中，我们可以创建多个线程
        // 并让每个线程执行不同的任务，从而实现并发处理。
        // 在C++中，线程函数必须是一个普通的全局函数或者是一个静态成员函数。
        // 这是因为线程函数实际上是一个由操作系统调用的全局函数。
        // 函数指针 int(*p)(int,int) 首先是一个指针 *p，其次前面的int表示这个指针变量可以指向返回值类型为int型的函数，后面括号中的两个int表示这个指针变量可以指向有两个参数而且都是int类型的函数
        // 即，定义了一个指针变量p，该指针变量可以指向返回值类型为int型，而且有两个整形参数的函数 p的类型为 int(*)(int,int)
        // 函数指针的定义方式为：
        // 函数返回值类型(* 指针变量名)(函数参数列表);
        
        // 定义工作线程运行的函数：
        static void *worker(void *arg);

        void run();
    
    private:
        //线程池中的线程数
        int m_thread_number;
        
        // 请求队列中允许的最大请求数
        int m_max_requests;

        // 描述线程池的数组，其大小为m_thread_number
        pthread_t *m_threads;

        //请求队列
        std::list<T*>m_workqueue;

        // 保护请求队列的互斥锁
        locker m_queuelocker;
        // 信号量对象 用于表示请求队列中是否有任务需要处理
        // 在append函数中，当有新的请求需要处理时，会对m_queuestat进行v操作，表示有任务可以执行
        // 在worker函数中，每个工作线程会不断进行p操作
        sem m_queuestat;
        // 是一个布尔类型的标志，用于表示线程池是否需要停止。
        // 构造函数中m_stop被初始化为false 表示线程池状态为运行中
        // 当需要关闭线程池时，可以讲m_stop设置为true 然后唤醒所有工作线程 使其退出
        // 为什么关闭线程池时要唤醒所有的工作线程？？
        // 当m_stop被设置为true后，对m_queuestat多次进行v操作，多次唤醒工作线程
        // 这样做的目的是，工作线程会在处理完当前的任务后发现 m_stop 已经被设置为 true，于是它们会退出循环，终止线程函数，从而退出线程。
        // 唤醒线程是为了确保他们能够及时赶至到线程池的关闭信号
        bool m_stop;

        // 指向数据库连接池的指针 是的线程池可以获取数据库连接池的信息
        // 当有新的请求到来时，会将请求添加到请求队列中，并通过这个指针获取数据库连接
        // 最后传递给工作线程来处理数据库请求
        connection_pool* m_connPool;

};

template<typename T>
threadpoll<T>::threadpoll(connection_pool *connPool,int thread_number,int max_request):m_thread_number(thread_number),m_max_requests(max_request),m_connPool(connPool),m_stop(false),m_thread(NULL){
    if(m_thread_number<=0||m_max_requests<=0){
        throw std::exception();
    }
    m_threads = new thread_t[thread_number];
    if(!m_threads){
        throw std::exception();
    }
    for(int i=0;i<thread_number;i++){
        // 构造函数里面就已经把线程开辟好了 还设为分离状态 然后每个线程就开始调用worker
        if(pthread_create(m_threads+i,NULL,worker,this)!=0){
            delete[] m_threads;
            throw std::exception();
        }
        if(pthread_detach(m_threads[i])>0){
            delete[] m_threads;
            throw std::exception();
        }
    }

}

template<typename T>
threadpoll<T>::~threadpoll(){
    delete[] m_threads;
    m_stop = true;
    
}

template<typename T>
bool threadpoll<T>::append(T* request){
    //appen a request
    // first lock
    m_queuelocker.lock();
    
    if(m_workqueue.size()>m_max_requests){
        m_queuelocker.unlock();
        return false;
    }

    m_workqueue.push_back(request);
    m_queuestat.post();
    m_queuelocker.unlock();
    return true;
}

template<typename T>
void* threadpoll<T>::worker(void *arg){
    // 构造函数中就开辟好了指定数量的线程 然后线程一次调用worker函数 传入的参数当前对象
    threadpoll *poll = arg;
    poll->run();
    return poll;
}

template<typename T>
void threadpoll<T>::run(){
    // zhu xun huan
    // 如果线程池不关闭 就一直循环执行

    // 从请求队列里面找一个请求 然后处理 
    // 请求队列里面的请求是main函数中通过epoll监听 连接套接字的状态
    // 当连接套接字的状态变为可读时，说明发来了新的http请求 这时候主函数中调用append 将这个http请求放入线程池对象的请求队列中
    // 请求队列中有数据了 这时候工作线程就会把数据取出来 这个数据就是http对象
    while(!m_stop){
        m_queuestat.wait();
        m_queuelocker.lock();
        
        if(m_workqueue.empty()){
            m_queuelocker.unlock();
            continue;
        }

        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();

        if(request==NULL) continue;
        // TODO connectionRAII

        //TODO request
        request->process();
    }
}
#endif 
