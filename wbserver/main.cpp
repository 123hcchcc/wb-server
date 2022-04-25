//������
//ʹ���̳߳�ʵ�ֵ�web������
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
#include"lst_time.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000
#define TIMESLOT 5


static int pipefd[2];
//����lst_time.h�е���������������ʱ��
static sort_timer_lst timer_list;
static int epollfd = 0;

extern int addfd(int epollfd, int fd, bool one_shot);
extern int removefd(int epollfd, int fd);

void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction ac;
    memset(&ac, '\0', sizeof(ac));
    ac.sa_handler = handler;
    if (restart)
    {
        ac.sa_flags |= SA_RESTART;//�ж�ϵͳ����ʱ�����ص�ʱ������ִ�и�ϵͳ����
    }
    sigfillset(&ac.sa_mask);//sa_mask�е��ź�ֻ���ڸ�acָ����źŴ�����ִ�е�ʱ��Ż����ε��źż���!
    assert(sigaction(sig, &ac, NULL) != -1);
}

//��ʾ���󣬹رո����ӵ��ļ�������
void show_error(int connfd, const char* info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

//���÷�����IO
int setnonblocking_main(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//�źŴ�����
void sig_handler(int sig)
{
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

void timer_handler()
{
    //��ʱ��������ʵ���Ͼ��ǵ���tick����
    timer_list.tick();
    //��Ϊһ��alarm����ֻ������һ��SIGALRM�źţ���������Ҫ���¶�ʱ���Բ��ϴ���SIGALRM�ź�
    alarm(TIMESLOT);
}

//��ʱ���ص���������ɾ���ǻ����socket�ϵ�ע���¼�
void cb_func(client_data* user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);

    close(user_data->sockfd);
    http_conn::m_user_count--;
    printf("close fd [%d] by time cb_func \n", user_data->sockfd);

}


void listen_pro()
{

}

void create_threadpool()
{
    //�����̳߳�
    threadpool<http_conn>* pool = NULL;
    try
    {
        pool = new threadpool<http_conn>(10, 20);
    }
    catch (...)
    {
        return 1;
    }
}

int main(int argc, char* argv[])
{
    if (argc <= 1)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }
    //const char* ip = argv[1];
    int port = atoi(argv[1]);

    //����SIGPIPE�ź�,SIG_IGN��ʾ�����źŵĺ�
    addsig(SIGPIPE, SIG_IGN);

    //�����̳߳�
    create_threadpool();

    //Ԥ��Ϊÿ�����ܵĿͻ����ӷ���һ��http_conn����
    http_conn* users = new http_conn[MAX_FD];
    assert(users);
    int users_conut = 0;

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);



    /*�������ŶϿ�
    #include <arpa/inet.h>
    struct linger {
����    int l_onoff;
����    int l_linger;
    };
    ���ֶϿ���ʽ��
    1. l_onoff = 0; l_linger����
    close()���̷��أ��ײ�Ὣδ����������ݷ�����ɺ����ͷ���Դ���������˳���
    2. l_onoff != 0; l_linger = 0;
    close()���̷��أ������ᷢ��δ������ɵ����ݣ�����ͨ��һ��REST��ǿ�ƵĹر�socket����������ǿ���˳���
    3. l_onoff != 0; l_linger > 0;
    close()�������̷��أ��ں˻��ӳ�һ��ʱ�䣬���ʱ�����l_linger��ֵ�������������ʱʱ�䵽��֮ǰ������
    ��δ���͵�����(����FIN��)���õ���һ�˵�ȷ�ϣ�close()�᷵����ȷ��socket�������������˳�������close()
    ��ֱ�ӷ��ش���ֵ��δ�������ݶ�ʧ��socket��������ǿ�����˳�����Ҫע���ʱ�����socket������������Ϊ�Ƕ�
    ���ͣ���close()��ֱ�ӷ���ֵ��
    */
    struct linger tmp = { 1, 1 };
    setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));

    int ret = 0;
    struct sockaddr_in  address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    //���ö˿ڸ���
    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret >= -1);

    ret = listen(listenfd, 5);
    assert(ret >= -1);




    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);

    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;


    //��ʱ�����
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);//����һ��sock�׽��֣��ֱ�Ϊpipefd[2]
    assert(ret != -1);
    setnonblocking_main(pipefd[1]);
    addfd(epollfd, pipefd[0], false);

    //�����źŴ�����
    addsig(SIGALRM, sig_handler);
    addsig(SIGTERM, sig_handler);
    bool stop_server = false;

    client_data* users_timer = new client_data[MAX_FD];
    bool timeout = false;
    alarm(TIMESLOT);







    while (!stop_server)
    {
        printf("epoll wait!\n");
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR))
        {
            printf("epoll failure!\n");
            break;
        }

        for (int i = 0; i < number; ++i)
        {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t clinet_addrlength = sizeof(client_address);
                printf("wait listen accpt!\n");
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &clinet_addrlength);
                printf("---------------------b��������connfd == [%d]--------------\n", connfd);
                printf("get listen accpt!\n");

                if (connfd < 0)
                {
                    printf("error is %d\n", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd, "Internet server busy");
                    continue;
                }
                //��ʼ���ͻ�������
                users[connfd].init(connfd, client_address);


                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;


                //������ʱ����������ص������볬ʱʱ�䣬Ȼ��󶨶�ʱ�����û����ݣ���󽫶�ʱ����ӵ�����timer_list��
                util_timer* timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;//��ʱʱ��Ϊ15s��15��������ر�����
                users_timer[connfd].timer = timer;
                timer_list.add_timer(timer);








            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                if (events[i].events & EPOLLRDHUP) printf("---------EPOLLRDHUP--------");
                if (events[i].events & EPOLLHUP) printf("---------EPOLLHUP--------");
                if (events[i].events & EPOLLERR) printf("---------EPOLLERR--------");
                printf("accpt error! and close fd \n");
                //������쳣ֱ�ӹر�����
                util_timer* timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);
                if (timer)
                {
                    timer_list.del_timer(timer);
                }
            }


            //һ���źŴ���������sig_headle������pipe[1]��д�����ݣ���ʱ���Դ���epoll
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (-1 == ret)
                {
                    //handle the error
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                        case SIGALRM:
                        {
                            //��timeout��������ж�ʱ������Ҫ����������������ʱ����������Ϊ��ʱ��������ȼ����Ǻܸߣ��������ȴ�����������Ҫ������
                            timeout = true;
                            break;
                        }
                        case SIGTERM:
                        {
                            stop_server = true;
                        }

                        }
                    }
                }
            }





            else if (events[i].events & EPOLLIN)
            {
                util_timer* timer = users_timer[sockfd].timer;
                printf("wait epollin accpt!\n");
                if (users[sockfd].read())
                {
                    pool->append(users + sockfd);//ֱ�Ӱ�����http_conn������ȥ���������Ķ��ǵ�ַ����+sockfd���Ǵ���ǰsock���ڵĶ���ѽ
                    if (timer)
                    {
                        timer_list.adjust_timer(timer);
                    }

                    //����ʱ��
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        printf("adjust timer by write!\n");
                        timer_list.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_list.del_timer(timer);
                    }
                    //users[sockfd].close_conn();
                }
            }



            else if (events[i].events & EPOLLOUT)
            {
                //������ʱ����������ص������볬ʱʱ�䣬Ȼ��󶨶�ʱ�����û����ݣ���󽫶�ʱ����ӵ�����timer_list��

                util_timer* timer = users_timer[sockfd].timer;
                if (timer)
                {
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    printf("adjust timer by write!\n");
                    timer_list.adjust_timer(timer);
                }



                printf("wait epollout accpt!\n");
                if (!users[sockfd].write())
                {
                    //������쳣ֱ�ӹر�����
                    //util_timer *timer = users_timer[sockfd].timer;
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_list.del_timer(timer);
                    }
                }
            }
            if (timeout)
            {
                timer_handler();
                timeout = false;
            }
        }
    }
    close(pipefd[0]);
    close(pipefd[1]);
    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;
    return 0;


}