#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "locker.h"
#include "log.h"
#include "lst_time.h"
#include "sql_conn.h"

class util_timer;
class sort_timer_lst;
//�̳߳��࣬��������Ϊģ������Ϊ�˴��븴��
template<typename T>
class threadpool
{
public:
    /*thread_number���̳߳����̵߳�������max_requests������������������ġ��ȴ���������������*/
    threadpool(connection_pool* connPool, int thread_number, int max_request, int m_actor_model);
    ~threadpool();
    bool append(T* request);

private:
    /*�����߳����еĺ����������ϴӹ���������ȡ������ִ��֮*/
    static void* worker(void* arg);
    void run();

private:
    int m_thread_number;        //�̳߳��е��߳���
    int m_max_requests;         //�����������������������
    pthread_t* m_threads;       //�����̳߳ص����飬���СΪm_thread_number
    std::list<T*> m_workqueue; //�������
    locker m_queuelocker;       //����������еĻ�����
    sem m_queuestat;            //�Ƿ���������Ҫ����
    bool m_stop;                //�Ƿ�����߳�
    int m_actor_model;          //��Ӧ��ģʽ 

    connection_pool* m_connPool;  //���ݿ����ӳ�

};
template <typename T>
threadpool<T>::threadpool(connection_pool* connPool, int thread_number, int max_requests, int actor_model) :
    m_thread_number(thread_number),
    m_max_requests(max_requests),
    m_stop(false), m_threads(NULL),
    m_actor_model(actor_model),
    m_connPool(connPool)
{
    LOG_INFO("��ʼ���̳߳أ�\n");
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    LOG_INFO("�����߳�\n");
    m_threads = new pthread_t[m_thread_number];
    LOG_INFO("�ж��߳�\n");
    if (!m_threads)
        throw std::exception();

    LOG_INFO("ѭ�������߳�\n");
    for (int i = 0; i < thread_number; ++i)
    {
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
    LOG_INFO("ѭ�������߳̽���\n");
}
template <typename T>
threadpool<T>::~threadpool()
{
    LOG_INFO("�̳߳�������\n");
    delete[] m_threads;
}
template <typename T>
bool threadpool<T>::append(T* request)
{
    LOG_INFO("append()\n");
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
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
void* threadpool<T>::worker(void* arg)
{

    threadpool* pool = (threadpool*)arg;
    pool->run();

    return pool;
}
template <typename T>
void threadpool<T>::run()
{

    while (true)
    {
        m_queuestat.wait();
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
        {
            continue;
        }

        if (m_actor_model == 1)
        {
            if (request->get_rw_state() == true)
            {
                if (request->read())
                {
                    //ֻ�ж�ȡ�ɹ���Ҫ���
                    LOG_INFO("ͨ��reactorģʽ��ȡ�ɹ�\n");

                    LOG_INFO("ȡ�����ݿ�����\n");
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    request->process();
                }
                else
                {
                    LOG_ERROR("write_thing error and close fd\n");
                    util_timer* timer = request->get_timer();

                    //ִ�лص�����������ر�����
                    timer->cb_func(request);

                    //����ָ��ǰ���п�
                    if (timer)
                    {
                        request->get_timer_list()->del_timer(timer);
                    }
                }
            }
            else
            {
                if (request->write())
                {

                }
                else
                {
                    LOG_ERROR("write_thing error and close fd\n");
                    util_timer* timer = request->get_timer();

                    //ִ�лص�����������ر�����
                    timer->cb_func(request);

                    //����ָ��ǰ���п�
                    if (timer)
                    {
                        request->get_timer_list()->del_timer(timer);
                    }
                }
            }
        }
        else
        {
            LOG_INFO("ȡ�����ݿ�����\n");
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }



    }
}

