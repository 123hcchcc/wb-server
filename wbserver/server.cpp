#include "server.h"

//��ʱ���ص���������ɾ���ǻ����socket�ϵ�ע���¼�
void cb_func(http_conn* user_data)
{
    epoll_ctl(http_conn::m_epollfd, EPOLL_CTL_DEL, user_data->get_sockfd(), 0);
    assert(user_data);

    close(user_data->get_sockfd());
    http_conn::m_user_count--;
    LOG_INFO("fd==[%d] closed by time cb_func \n", user_data->get_sockfd());


}

//�źŴ�����
void sig_handler(int sig)
{
    int save_errno = errno;
    int msg = sig;
    send(m_pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}


//��ʼ��
server::server(int port, int thread_number, int thread_task_number,
    int trig_model, int actor_model, string user,
    string password, string dataBaseName) :
    m_port(port),
    m_thread_number(thread_number),
    m_thread_task_number(thread_task_number),
    m_stop_server(false),
    m_close_log(0),
    m_is_block(1),
    m_trig_model(trig_model),
    m_actor_model(actor_model),
    m_user(user),
    m_passWord(password),
    m_databaseName(dataBaseName)
{

}

//��ʼ����־
void server::init_log()
{
    if (0 == m_close_log)
    {
        //��ʼ����־
        if (1 == m_is_block)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);//�첽��־
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);//ͬ����־
    }
}

//��ʼ�����ݿ����ӳ�
void server::sql_pool()
{
    printf("zhunbei init sql\n");
    //��ʼ�����ݿ����ӳ�
    m_connPool = connection_pool::GetInstance();
    printf("init sql\n");
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, 8, m_close_log);

    //��ʼ�����ݿ��ȡ��
    printf("init sql mysql\n");
    users->initmysql_result(m_connPool);
    printf("init sql end\n");
}

//�ͷŶ����Ŀռ�
server::~server()
{

    close(m_pipefd[0]);
    close(m_pipefd[1]);
    close(m_epollfd);
    close(m_listenfd);
    //delete[] m_users_timer;
    delete[] users;
    delete m_pool;

}

// void server::init(int port,int thread_number = 10, int thread_task_number = 20, bool stop_server = false)
// {
//     m_port = port;
//     m_thread_number = thread_number;
//     m_thread_task_number = thread_task_number;
//     m_stop_server = stop_server;
//     m_timeout = false;
// }m_connPool



//��ʼ���̳߳�
bool server::create_threadpool()
{
    //�����̳߳�
    m_pool = NULL;
    try
    {
        m_pool = new threadpool<http_conn>(m_connPool, m_thread_number, m_thread_task_number, m_actor_model);
        LOG_INFO("�����̳߳����\n");
    }
    catch (...)
    {
        LOG_ERROR("�̳߳ش���\n");
        return false;
    }

    return true;
}

//�����������������飬���ڹ�������
void server::create_http_conn()
{
    //Ԥ��Ϊÿ�����ܵĿͻ����ӷ���һ��http_conn����
    users = new http_conn[MAX_FD];
    assert(users);
    LOG_INFO("http_conn[]������ɣ�\n");

}


//���������ļ�������
void server::create_listen()
{

    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    //���Źر�
    struct linger tmp = { 1, 1 };
    setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));


    //���ñ���socket
    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(m_port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);


    //���ö˿ڸ���
    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    //��
    ret = bind(m_listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret >= 0);

    //����
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);
    LOG_INFO("���������ɹ���fd==[%d]\n", m_listenfd);


}

//����epoll���������ļ�����������
void server::create_epoll()
{

    //����events�������ڽ���events�¼�
    epoll_event events[MAX_EVENT_NUMBER];

    //����epoll�ļ��������������+˫������
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    //�������ļ�����������
    addfd(m_epollfd, m_listenfd, false, m_trig_model);
    http_conn::m_epollfd = m_epollfd;
    LOG_INFO("epoll������ɣ�listen�����ɹ���\n");
}


//������ʱ��
void server::create_timer()
{
    //��ʱ�����
    int ret;

    //ͳһ�¼�Դ������һ��sock�ܵ������źŵĴ���д��һ�ˣ���һ��ʹ��epoll����
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);//����һ��sock�׽��֣��ֱ�Ϊpipefd[2]

    assert(ret != -1);
    this->setnonblocking(m_pipefd[1]);
    addfd(m_epollfd, m_pipefd[0], false, m_trig_model);

    //�����źŴ�����

    //�������writeд���ѹرյ����Ӳ���SIGPIPE�ź��жϽ���
    addsig(SIGPIPE, SIG_IGN, true);

    //����������ʱ�䴥���źŰ��źŴ�����
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);

    //������ֹͣ���б�־
    bool m_stop_server = false;

    //�����ͻ��������飬���а����˿ͻ���Ӧ�Ķ�ʱ����Ϣ
    //m_users_timer = new client_data[MAX_FD];

    //5����֮�󴥷�ALARM�ź�->sig_headler->tick()�����������
    alarm(TIMESLOT);
}

//����һ������
bool server::accpt_thing()
{
    struct sockaddr_in client_address;
    socklen_t clinet_addrlength = sizeof(client_address);
    int connfd;


    //ETģʽ��Ҫѭ����ȡ����Ŷ
    if (m_trig_model == 1)
    {
        while (1)
        {
            connfd = accept(m_listenfd, (struct sockaddr*)&client_address, &clinet_addrlength);
            LOG_DEBUG("��������fd==[%d]\n", connfd);
            if (connfd <= 0)
            {
                LOG_DEBUG("error is %d\n", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD)
            {
                show_error(connfd, "Internet server busy");
                break;
            }
            client_init(connfd, client_address);
        }
        return false;
    }
    else
    {
        connfd = accept(m_listenfd, (struct sockaddr*)&client_address, &clinet_addrlength);
        LOG_DEBUG("��������fd==[%d]\n", connfd);
        if (connfd < 0)
        {
            LOG_DEBUG("error is %d\n", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD)
        {
            show_error(connfd, "Internet server busy");
            return false;
        }
        client_init(connfd, client_address);
    }



    return true;//LTģʽ����true��ʾ���̼�����ETģʽ�������ȥ����һ��

}

void server::client_init(int connfd, struct sockaddr_in client_address)
{
    //��ʼ���ͻ�������
    users[connfd].init(connfd, client_address, m_trig_model, &m_timer_list);

    //�����Ӽ�¼���ͻ������ϣ��Ա��ʱ���رշǻ�Ծ����
    //m_users_timer[connfd].address = client_address;
    //m_users_timer[connfd].sockfd = connfd;

    //������ʱ����������ص������볬ʱʱ�䣬Ȼ��󶨶�ʱ�����û����ݣ���󽫶�ʱ����ӵ�����timer_list��
    util_timer* timer = new util_timer;
    timer->user_data = &users[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;//��ʱʱ��Ϊ15s��15��������ر�����
    users[connfd].set_timer(timer);
    m_timer_list.add_timer(timer);
}

//���ڹر����ӣ�Ҳ����������������ر�����
void server::timer_over(int sockfd)
{
    util_timer* timer = users[sockfd].get_timer();

    //ִ�лص�����������ر�����
    timer->cb_func(&users[sockfd]);

    //����ָ��ǰ���п�
    if (timer)
    {
        m_timer_list.del_timer(timer);
    }
}

//�����ź�
bool server::time_signal()
{
    int ret;
    int sig;

    //�ź����飬���ڽ����ź�
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (-1 == ret)
    {
        LOG_ERROR("�ӹܵ��н����ź�ʧ�ܣ�����ʧ�ܣ���\n");
        return false;
    }
    else if (ret == 0)
    {
        LOG_ERROR("�ӹܵ��н����ź�ʧ�ܣ��ܵ��������ݣ�\n");
        return false;
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
                m_timeout = true;
                break;
            }
            case SIGTERM:
            {
                LOG_ERROR("SIGTERM�źŴ������˳����̣�\n");
                m_stop_server = true;
            }

            }
        }
    }
}


//������¼�
void server::read_thing(int sockfd)
{
    LOG_DEBUG("read_thing\n");
    util_timer* timer = users[sockfd].get_timer();


    if (m_actor_model == 1)
    {

        updata_time(sockfd);
        //�����̳߳ش���
        users[sockfd].set_read();
        m_pool->append(users + sockfd);

    }
    else
    {
        //proactorģʽ����д�����������̣߳����������ڹ����߳�
        if (users[sockfd].read())
        {
            //ֱ�Ӱ�����http_conn������ȥ���������Ķ��ǵ�ַ����+sockfd���Ǵ���ǰsock���ڵĶ���ѽ
            m_pool->append(users + sockfd);

            if (timer)
            {
                m_timer_list.adjust_timer(timer);
            }
            //���¼�ʱʱ��
            updata_time(sockfd);
        }
        else
        {
            LOG_ERROR("read()����ر����ӣ�\n");
            timer->cb_func(&users[sockfd]);
            if (timer)
            {
                m_timer_list.del_timer(timer);
            }

        }
    }

}

//д�¼�
void server::write_thing(int sockfd)
{
    LOG_DEBUG("write_thing\n");

    if (m_actor_model == 1)
    {
        updata_time(sockfd);
        users[sockfd].set_write();
        m_pool->append(users + sockfd);

    }
    else
    {
        if (users[sockfd].write())
        {
            //д��ɹ�������Ϊ��Ծ����
            updata_time(sockfd);
        }
        else
        {
            LOG_ERROR("write_thing error and close fd = [%d]\n", sockfd);
            timer_over(sockfd);
        }
    }

}


//��ѭ��
void server::main_loop()
{
    // /LOG_INFO("main_loop!\n");
    m_timeout = false;
    m_stop_server = false;
    while (!m_stop_server)
    {
        LOG_DEBUG("main_loop!\n");
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR))
        {
            printf("epoll failure!\n");
            break;
        }

        //�����¼�
        for (int i = 0; i < number; ++i)
        {
            int sockfd = events[i].data.fd;

            //����������
            if (sockfd == m_listenfd)
            {
                bool flag = accpt_thing();
                if (false == flag)
                {
                    continue;
                }
            }


            //error�¼�
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                LOG_INFO("fd=[%d] ��������", sockfd);
                if (events[i].events & EPOLLRDHUP) LOG_INFO("EPOLLRDHUP�¼�");
                if (events[i].events & EPOLLHUP) LOG_INFO("EPOLLHUP�¼�");
                if (events[i].events & EPOLLERR) LOG_INFO("EPOLLERR�¼�");
                LOG_INFO("\n");

                //������쳣ֱ�ӹر�����
                timer_over(sockfd);
            }


            //һ���źŴ���������sig_headle������pipe[1]��д�����ݣ���ʱ���Դ���epoll
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                //LOG_INFO("signal�¼�:\n");
                int flag = time_signal();
                if (false == flag)
                {
                    continue;
                }
            }


            else if (events[i].events & EPOLLIN)
            {
                LOG_INFO("epollin�¼�:\n");
                read_thing(sockfd);
            }

            else if (events[i].events & EPOLLOUT)
            {
                //������ʱ����������ص������볬ʱʱ�䣬Ȼ��󶨶�ʱ�����û����ݣ���󽫶�ʱ����ӵ�����timer_list��
                LOG_INFO("д�¼�\n");
                write_thing(sockfd);

            }
            if (m_timeout)
            {
                //LOG_INFO("��ʱ�¼���\n");
                timer_handler();
                m_timeout = false;
            }
        }
    }
}


//����sockfd�ϵļ�ʱ��
void server::updata_time(int sockfd)
{
    LOG_INFO("����Ϊ��Ծ����fd=[%d]\n", sockfd);
    util_timer* timer = users[sockfd].get_timer();
    if (timer)
    {
        time_t cur = time(NULL);
        timer->expire = cur + 3 * TIMESLOT;
        //printf("adjust timer by write!\n");
        m_timer_list.adjust_timer(timer);
    }
}



void server::addsig(int sig, void(handler)(int), bool restart)
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
void server::show_error(int connfd, const char* info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

//���÷�����IO
int server::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}




void server::timer_handler()
{
    //LOG_DEBUG("����ʱ����\n");
    //��ʱ��������ʵ���Ͼ��ǵ���tick����
    m_timer_list.tick();
    //��Ϊһ��alarm����ֻ������һ��SIGALRM�źţ���������Ҫ���¶�ʱ���Բ��ϴ���SIGALRM�ź�
    alarm(TIMESLOT);
}
