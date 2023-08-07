#ifndef LOCKER_H
#define LOCKER_H
#include <exception>
#include <pthread.h>
#include <semaphore.h>
// locker中主要是对linux下的三种锁进行封装 将锁的创建和销毁函数封装在类的构造与析构函数中 实现RAII
// 当然可以直接用这三种锁，但是直接使用并不是面向对象的做法

class sem{
    public:
        // 构造函数
        sem();
        // 析构函数
        ~sem();
        bool wait();
        bool post();
    private:
        sem_t m_sem;
    
};
sem::sem(){
    //信号量初始化
    if(sem_init(&m_sem,0,0)!=0){
        throw std::exception();
    }
}

sem::~sem(){
    //信号量销毁
    sem_destroy(&m_sem);
}

bool sem::wait(){
    return sem_wait(&m_sem)==0;
}

bool sem::post(){
    return sem_post(&m_sem)==0;
}

class locker{
    private:
        pthread_mutex_t m_mutex;
    public:
        locker(){
            if(pthread_mutex_init(&m_mutex,NULL)!=0)
            {
                throw std::exception();
            }
        }
        ~locker(){
            pthread_mutex_destroy(&m_mutex);
        }
        bool lock();
        bool unlock();
        pthread_mutex_t * get();
};

bool locker::lock(){
    return pthread_mutex_lock(&m_mutex)==0;
}

bool locker::unlock(){
    return pthread_mutex_unlock(&m_mutex)==0;
}

pthread_mutex_t * locker::get(){
    return &m_mutex;
}

class cond{
    private:
        pthread_cond_t m_cond;
    public:
        cond(){
            if(pthread_cond_init(&m_cond,NULL)!=0){
                throw std::exception();
            }
        }
        ~cond(){
            pthread_cond_destroy(&m_cond);
        }

        bool wait(pthread_mutex_t *m_mutex){
            int ret;
            ret = pthread_cond_wait(&m_cond,m_mutex);
            return ret==0;
        }
        
        // 等待条件变量的满足，并在指定的时间段内等待 直到条件满足或者超时
        bool timewait(pthread_mutex_t *m_mutex,struct timespec t){
            int ret = 0;
            ret = pthread_cond_timedwait(&m_cond,m_mutex,&t);
            return ret==0;

        }

        bool signal(){
            return pthread_cond_signal(&m_cond)==0;
        }

        bool broadcast(){
            return pthread_cond_broadcast(&m_cond)==0;
        }
};
#endif 