
#ifndef _LST_TIME_H
#define _LST_TIME_H

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
#include <time.h>


#include "log.h"
#include "http_conn.h"
#define BUFFER_SIZE 64

class util_timer;//��ǰ����
class http_conn;


//�ͻ������ӵ����ݣ�ÿһ���ͻ���������������¼��fd�Ͷ�ʱ��
// struct client_data
// {
//     sockaddr_in address;
//     int sockfd;
//     //char buf[BUFFER_SIZE];
//     util_timer * timer;

// };

//��ʱ����
class util_timer
{
public:
    util_timer() :prev(NULL), next(NULL) {}

public:
    time_t expire;//����ĳ�ʱʱ�䣬����ʹ�þ���ʱ��
    void (*cb_func)(http_conn*);//����ص��������ص���������Ŀͻ����ݣ��ɶ�ʱ����ִ���ߴ��ݸ��ص�����
    http_conn* user_data;
    util_timer* prev;//ָ��ǰһ����ʱ��
    util_timer* next;//ָ���һ����ʱ��    

};

//��ʱ����������һ������˫�������Ҵ���ͷ�ڵ��β�ڵ�
class sort_timer_lst
{
public:

    sort_timer_lst() :head(NULL), tail(NULL) {}//��ʼ��

    ~sort_timer_lst();//��������ʱ��ɾ���������нڵ�

    void add_timer(util_timer* timer);//��˳����Ӽ�ʱ��

    void adjust_timer(util_timer* timer); //��ĳһ���������任ʱ��Ӧ�õ�����Ӧ�Ķ�ʱ���������е�λ�á��������ֻ���Ǳ������Ķ�ʱ���ĳ���ʱ���ӳ�����������ö�ʱ����Ҫ��β���ƶ�

    void del_timer(util_timer* timer); //��Ŀ�궨ʱ��timer��������ɾ��

    /*Ҳ���Ǳ���������ʱ��ȫ��ִ�лص�����*/
    void tick();//SIGALRM�ź�ÿ�α������������źŴ����������ʹ��ͳһ�¼�Դ���Ǿ�������������ִ��һ��tick�������Ѵ��������ϵ��ڵ�����


private:
    //һ�����صĸ�������,��add_timer�������ʱ������øú����������
    void add_timer(util_timer* timer, util_timer* lst_head);

private:
    util_timer* head;
    util_timer* tail;

};

