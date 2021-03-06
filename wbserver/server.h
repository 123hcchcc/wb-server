//将main函数的主要步骤封装起来
#ifndef _SERVER_H
#define _SERVER_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>

#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include "lst_time.h"
#include "log.h"
#include "sql_conn.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000
#define TIMESLOT 5
extern int addfd(int epollfd, int fd, bool one_shot, int trig_model);
extern int removefd(int epollfd, int fd);
static int m_pipefd[2];//信号管道，上树监听信号

class server
{
public:
    server() {};
    server(int port, int thread_number, int thread_task_number, int trig_model,
        int actor_model, string user, string password, string dataBaseName);
    ~server();
    //void init(int port,int thread_number = 10, int threak_task_number = 20);
    bool create_threadpool();

    void create_http_conn();

    void init_log();
    void create_listen();

    void create_epoll();

    void create_timer();

    void main_loop();

    bool accpt_thing();

    void timer_over(int sockfd);

    bool time_signal();

    void read_thing(int sockfd);

    void write_thing(int sockfd);


    void updata_time(int sockfd);

    //在函数的声明和定义中只能有一处指定参数的默认值
    void addsig(int sig, void(handler)(int), bool restart);

    void show_error(int connfd, const char* info);

    int setnonblocking(int fd);

    //void sig_handler(int sig);

    //void cb_func(client_data* user_data);

    void timer_handler();

    void client_init(int connfd, struct sockaddr_in client_address);

    void sql_pool();

private:

    int m_port;//端口号
    int m_epollfd;//如名
    int m_listenfd;//如名

    //client_data * m_users_timer;//计时器&文件描述符的数据数组

    int m_thread_number;//最大线程数量
    int m_thread_task_number;//最大任务数量
    epoll_event events[MAX_EVENT_NUMBER];//接受事件的数组

    sort_timer_lst m_timer_list;//计时器链表

    http_conn* users;//链接数组
    bool m_stop_server;
    bool m_timeout;
    threadpool<http_conn>* m_pool;

    //日志
    int m_close_log;
    int m_is_block;

    //模式选择
    int m_trig_model;//触发模式：ET：1、LT：2
    int m_actor_model;//反应堆模式：proactor：0；reactor：1

    //数据库相关
    connection_pool* m_connPool;

    string m_user;         //登陆数据库用户名
    string m_passWord;     //登陆数据库密码
    string m_databaseName; //使用数据库名

    int m_sql_num;


};

