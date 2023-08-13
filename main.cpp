#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./timer/lst_timer.h"
#include "./http/http_conn.h"
#include "./log/log.h"
#include "./CGImysql/sql_connection_pool.h"

#define MAX_FD 65536            //最大文件描述符
#define MAX_EVENT_NUMBER 10000  //最大事件数
#define TIMESLOT 5              //最小超时单位

#define SYNLOG                  //同步写日志 TODO


#define listenfdLT              //阻塞LT模式

int main(int argc,char *argv[])
{
    // std::cout<<"myTinyWebServer是用来复现tinyWebServer的"<<std::endl;
    // std::cout<<"this is a test for branch dev"<<std::endl;
    // return 0;
    
    //TODO日志系统
#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8); //异步日志模型
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 0); //同步日志模型
#endif

    if(argc <= 1)
    {
        printf("usage error!");
        return 1;
    }

    int port = atoi(argv[1]);

    addsig(SIGPIPE, SIG_IGN);

    // 创建数据库连接池TODO

    // 创建线程池 TODO
    threadpool<http_conn> *pool = NULL;
    try
    {   
        // 为什么线程池构造的时候要穿入数据库连接池做参数
        pool = new threadpool<http_conn>(connPool);
    }
    catch(...)
    {
        return 1;
    }
    // 主线程创建多个http类对象 
    http_conn *users = new http_conn[MAX_FD];
    assert(users);
    // 开始小连招
    // 创建套接字 设置地址 绑定 监听 
    int listenfd = socket(PF_INET,SOCK_STREAM,0);
    assert(listenfd >= 0);

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address,sizoef(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    int flag = 1;

    // 设置套接字选项 SO_REUSEADDR表示允许地址重用 在套接字关闭后，允许快速绑定相同的地址和端口？？？ 
    // 意义？？？ TODO
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&flag,sizeof(flag));

    // 创建内核事件表 epoll_create 
    epoll_event events[MAX_EVENT_NUMBER];
    
    epollfd = epoll_create(5);
    assert(epollfd != -1);
    
    
    // 使用epoll_ctl 将监听套接字放入epollfd中 （使用addfd封装了epoll_ctl）
    addfd(epollfd,listenfd,false);
    // 开始while(ture)
    // number = epoll_wait()
    // for(i:number)
    // 如果是监听套接字状态改变 说明有新的客户端尝试连接 那么应该生成连接套接字(accept) 并放入epoll中
    // 客户端尝试连接 那就有http请求了 所以要创建一个http实例
    
    while(true)
    {
        int number = epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);
        
        for(int i=0;i<number;i++)
        {
            int sockfd = events[i].data.fd;

            if(sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);


                // 接收http请求生成连接套接字

                int connfd = accept(listenfd,(struct sockaddr*)&client_address,&client_addrlength);
                if(connfd < 0)
                {
                    // 日志TODO    
                    continue;
                }   
                if(http_conn::m_user_count >= MAX_FD)
                {
                    //TODO
                    continue;
                }

                // 接收连接请求 根据套接字初始化对应的http数组元素
                users[connfd].init(connfd,client_address);
                // 将连接套接字放入epoll中 有读写事件就要通过对应的http对象来处理
                
                //初始化client_data数据
                //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                // users_timer[connfd].address = client_address;
                // users_timer[connfd].sockfd = connfd;
                // util_timer *timer = new util_timer;
                // timer->user_data = &users_timer[connfd];
                // timer->cb_func = cb_func;
                // time_t cur = time(NULL);
                // timer->expire = cur + 3 * TIMESLOT;
                // users_timer[connfd].timer = timer;
                // timer_lst.add_timer(timer);

            }
            
            // else if(1 服务器端关闭连接 2 处理管道信号 TODO )
            
            // 处理客户连接上接收到的数据
            else if(events[i].events & EPOLLIN)
            {
                //timer TODO
                
                // 主要目的是 把http请求放入对应的请求队列中
                pool->append(users + sockfd);
            }
            
            // 处理可写状态的套接字
            else if(event[i].events & EPOLLOUT)
            {
                // timer TODO

                // 核心是调用对应HTTP对象的write函数 完成响应报文
                if(users[sockfd].write())
                {

                }
                else
                {

                }

            }

        }

        // if(timeout) TODO

        close(epollfd);
        close(listenfd);
        // close(pipefd[1]);
        // close(pipefd[0]);
        
    }
    // 要给这个新的连接套接字设置定时器 回调函数。。TODO

    // 如果是连接套接字状态改变 
    // 1.events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR) 依次表示：对端关闭连接（发送了fin数据包）  发生了挂起事件  发生了错误事件 ——————服务器端关闭连接
    // 2.events[i].events & EPOLLIN 该连接套接字有数据可供读取 
    //   处理客户连接上接收到的数据 
    //   调用read_once()

    delete[] users;
    delete[] users_timer;
    // 只有一个线程池 线程池中有成员是 指向数据库连接池的指针
    delete pool;
    return 0;
}