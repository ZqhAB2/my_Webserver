#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"



// 该文件从上之下依次定义的类和函数：
// 线程池类
// 类的构造函数
// 类的析构函数
// 线程类实现新任务添加和睡眠线程唤醒的函数
// 静态回调函数worker
// 真正的功能执行函数run



// 模板类中的T用于指定类中一些变量的类型，这样方便扩展类在不同数据类型上的通用性
template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();

    // 向请求队列中插入任务请求，T是表征任务的数据结构类型，实际实现时让T  = http_conn
    bool append(T *request);



// 这两个成员函数被设置成private
// 这样只有类内部的函数可以调用他们，在类外用户不可以通过对象来调用这两个函数
// 实现封装保护，更安全
private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    // 解析：此worker函数地址在后面的pthread_create中作为参数传入，因此要设置为静态成员函数
    

    // 返回值类型和参数类型时void* 可以增加函数对于不同数据类型的通用性，即泛型编程的思想
    // 泛型思想：在编写函数或类时不指定参数的类型，让用户在使用时在自己确定
    static void *worker(void *arg);

    // 真正的任务执行代码在run()函数中，worker函数只是为了调用run函数
    // 那么为什么要用worker()函数来调用run函数，而不是直接执行run函数呢？
    // 这是因为写死的库函数pthread_create()的第三个参数只接受唯一的一个void* 参数
    // 而传入非静态成员函数的指针时，会自动把this指针夹带传入，这样就相当于把两个参数塞给pthread_create()的void*的位置
    // 会报错，因此就采用静态类成员函数，但是为什么不直接把run定义成static的呢？因为run在执行时必然需要访问类中的非静态成员
    // 如果设置成静态成员函数，就无法访问非静态成员变量和非静态成员函数了
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列，使用链表实现
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //是否有任务需要处理
    bool m_stop;                //是否结束线程
    connection_pool *m_connPool;  //数据库
};



// 线程池类的构造函数的具体实现
template <typename T>
// 下面这行，使用初始化列表来对类中的成员进行初始化，即将参数列表承接到的数值赋给冒号后的各个成员变量
threadpool<T>::threadpool( connection_pool *connPool, int thread_number, int max_requests) : m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(NULL),m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    // 这个数组中存储了m_thread_number个pthread_t型变量，一个pthread_t就是一个线程对应的id
    m_threads = new pthread_t[m_thread_number];

    // 如果创建线程池数组没有成功，则报错
    if (!m_threads)
        throw std::exception();
    

    // 开始正式创建线程池
    for (int i = 0; i < thread_number; ++i)
    {
        //printf("create the %dth thread\n",i);
        
        // 解析：此处的pthread_create函数中，第三个参数worker，为函数指针，指向处理线程函数的地址
        // 该函数，要求为静态函数。如果处理线程函数为类成员函数时，需要将其设置为静态成员函数
        
        // pthread_create函数中将  threadpool类   的对象的this指针作为参数传递给静态函数(worker),
        // 在静态函数中引用这个对象,并调用其动态方法(run)。
        
        // 下面参数列表中的this跟在worker后面，意思是把this作为参数传递给worker
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }

        // 创建线程成功后，要将线程分离，这样，线程执行完后其资源会被系统自动回收，而不需要其他线程来join回收
        // pthread_detach返回非零，则说明detach失败
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;     // 释放m_threads所指向的   pthread_t类型数组    所占用的堆空间
            throw std::exception();
        }
    }
}



template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
    m_stop = true;
}


// 当有新的客户请求到来时，线程池对象收到主线程的通知后
// 会把新的任务插入到list<T*>中，然后使用m_queuestat信号量来通知池子里的线程过来领取任务
template <typename T>
bool threadpool<T>::append(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() > m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();

    // post()函数是让信号量加1，这样其他阻塞在m_queuestat.wait();语句的线程才能向下执行，否则在池子里sleep
    m_queuestat.post();
    return true;
}



// 这个函数只是实现从静态回调函数worker来启动真正的工作处理函数run的目的
// 解释：之所以需要这个函数是因为不能直接给C库里的pthread_create（屎山）函数传入非静态成员函数的函数指针
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    // 传进来的arg是threadpool<T>类型的this指针
    // 这个this指针是在构造函数中调用pthread_create函数时，由pthread_create函数
    // 传递给worker函数的，即arg
    threadpool *pool = (threadpool *)arg;

    // 此处的pool就是在主函数中创建的threadpool类的对象的this指针
    pool->run();
    return pool;
}



// 这个run函数是每个工作线程真正执行的内容
template <typename T>
void threadpool<T>::run()
{
    // create了之后，每个线程就会一直处于while循环状态，只不过没有任务时，
    // 会睡在m_queuestat.wait();语句处，等待新任务到来并被唤醒
    while (!m_stop)
    {
        // m_queuestat是一个sem，即信号量，wait操作是P操作，信号量减1
        m_queuestat.wait();
        // 这个锁，保护的是任务队列
        m_queuelocker.lock();
        // 如果任务队列中没有任务，就解锁，并再次循环
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }

        // 如果任务队列中有任务，就取出第一个任务，然后解锁，解锁后，其他的线程才能访问任务队列这一公共资源
        // 一个T就是任务队列中的一个任务   T*是任务数据结构的地址
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
            continue;

        // 从连接池中取出一个数据库连接
        connectionRAII mysqlcon(&request->mysql, m_connPool);
        
        // 启动process函数来完成http请求报文的解析和请求的响应
        request->process();
    }
}
#endif
