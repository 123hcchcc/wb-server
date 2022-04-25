#include <mysql/mysql.h>
#include "http_conn.h"
#include <fstream>

//����HTTP��Ӧ��״̬��Ϣ
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "you do not have permission to get file form this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";



//��վ�ĸ�Ŀ¼
const char* doc_root = "/home/chendongyu/root";

//map<string, string> users;
locker m_lock;

//���ݿ����
void http_conn::initmysql_result(connection_pool* connPool)
{
    //�ȴ����ӳ���ȡһ������
    printf("�ȴ����ӳ���ȡһ������\n");
    MYSQL* mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //��user���м���username��passwd����
    printf("��user���м���username��passwd����\n");
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //�ӱ��м��������Ľ����
    printf("�ӱ��м��������Ľ����\n");
    MYSQL_RES* result = mysql_store_result(mysql);

    //���ؽ�����е�����
    printf("���ؽ�����е�����\n");
    if (result == NULL) printf("result wei kong\n");
    int num_fields = mysql_num_fields(result);

    //���������ֶνṹ������
    printf("���������ֶνṹ������\n");
    MYSQL_FIELD* fields = mysql_fetch_fields(result);

    //�ӽ�����л�ȡ��һ�У�����Ӧ���û��������룬����map��
    printf("�ӽ�����л�ȡ��һ�У�����Ӧ���û��������룬����map��\n");
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        m_users[temp1] = temp2;
    }
}







//����Ϊ�������ļ�������
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//����
void addfd(int epollfd, int fd, bool one_shot, int trig_model)
{
    epoll_event event;
    event.data.fd = fd;
    if (trig_model == 1)
    {
        event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
    }
    else
    {
        event.events = EPOLLIN | EPOLLRDHUP;//EPOLLRDHUP�Զ˶Ͽ�����
    }

    if (one_shot)
    {
        event.events |= EPOLLONESHOT;
    }
    LOG_INFO("addfd��fd:[%d]\n", fd);
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//�������Ƴ��ļ�������
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//�޸����ϵ��ļ�������
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    //event.events = ev | EPOLLET | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);

}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//�ر�����
void http_conn::close_conn(bool real_close)
{
    LOG_DEBUG("close fd==[%d] by close_conn", m_sockfd);
    if (real_close && (m_sockfd != -1))
    {
        LOG_INFO("closed fd == [%d]", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//�����ʼ��
void http_conn::init(int sockfd, const sockaddr_in& addr, int trig_model, sort_timer_lst* timer_list)
{

    m_timer_list = timer_list;

    m_sockfd = sockfd;
    m_address = addr;
    char _ip[64];
    inet_ntop(AF_INET, &m_address.sin_addr, _ip, sizeof(_ip));
    LOG_INFO("���� ip = [%s] fd == [%d]\n", _ip, sockfd);

    m_trig_model = trig_model;
    //����������Ϊ�˱���TIME_WAIT״̬�������ڵ��ԣ�ʵ��ʹ��ʱӦ��ȥ��,Ҳ���Ƕ˿ڸ���
    // int reuse = 1;
    // setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    addfd(m_epollfd, sockfd, true, trig_model);
    ++m_user_count;


    //strcpy(sql_user, user.c_str());
    //strcpy(sql_passwd, passwd.c_str());
    //strcpy(sql_name, sqlname.c_str());


    init();
}

void http_conn::init()
{
    m_checked_state = CHECK_STATE_REQUESTION;
    m_linger = false;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    memset(m_real_file, '\0', sizeof(m_real_file));
    memset(m_read_buf, '\0', sizeof(m_read_buf));
    memset(m_write_buf, '\0', sizeof(m_write_buf));

    m_trig_model = 0;
    m_rw_state = 0;
    mysql = NULL;
}


//��״̬�������ڽ�����һ������
http_conn::LINE_STATUS http_conn::parse_line()
{

    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        //��õ�ǰ��������
        temp = m_read_buf[m_checked_idx];
        //�����ǰ���ֽ���\r�����س�������˵�����ܶ�ȡ��һ����������
        if (temp == '\r')
        {
            //���\r�ַ�������Ŀǰbuffer�е����һ���Ѿ�������Ŀͻ����ݣ���ô��η���û�ж�ȡ��һ���������У���Ҫ������ȡ����
            if (m_checked_idx + 1 == m_read_idx)
            {
                return LINE_OPEN;
            }

            //��ʾ������һ����������
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {

                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            LOG_DEBUG("pares_line()�﷨����\n");
            //���϶����ǣ����Ǵ����﷨����
            return LINE_BAD;

        }
        //��ǰ�ַ�Ϊ\nҲ�п����ǵ���һ�е����
        else if (temp == '\n')
        {
            //��Ϊ\r\nһ���ã������ж�
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {

                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            LOG_ERROR("pares_line() error\n");
            return LINE_BAD;
        }

    }
    //������е��ַ��������˻�û������
    return LINE_OPEN;

}

//ѭ����ȡ�ͻ������ݣ�֪�������ݿɶ����߶Է��ر�����
bool http_conn::read()
{
    LOG_DEBUG("read()\n");
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;
    while (true)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
            READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)//recv����EWOULDBLOCK��ʾ����������
            {
                if (errno == EAGAIN) LOG_ERROR("read() EAGAIN\n");
                if (errno == EWOULDBLOCK) LOG_ERROR("read() EWOULDBLOCK\n");
                break;
            }
            return false;
        }
        else if (bytes_read == 0)
        {
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
}

//����HTTP�����У���ȡ���󷽷���Ŀ��URL�Լ�HTTP�汾��
http_conn::HTTP_CODE http_conn::parse_request_line(char* text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        LOG_ERROR("[-----%d-----]BAD_REQUEST[m_url]\n", m_sockfd);
        return BAD_REQUEST;
    }
    *m_url++ = '\0';

    char* method = text;
    if (strcasecmp(method, "GET") == 0)
    {
        LOG_ERROR("[-----%d-----]m_method get!\n", m_sockfd);
        m_method = GET;
    }
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        m_is_post = 1;
    }
    else
    {
        LOG_ERROR("[-----%d-----]BAD_REQUEST[m_method]\n", m_sockfd);
        return BAD_REQUEST;
    }
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
    {
        LOG_ERROR("[-----%d-----]BAD_REQUEST[m_version]\n", m_sockfd);
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");


    if (strcasecmp(m_version, "HTTP/1.1") != 0)
    {

        LOG_ERROR("BAD_REQUEST[m_version is not http1.1]\n");
        return BAD_REQUEST;
    }
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        LOG_ERROR("BAD_REQUEST[m_url is not http:]\n");
        m_url += 7;
        m_url = strchr(m_url, '/');

    }
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/')
    {
        LOG_ERROR("BAD_REQUEST[m_url��m_url[0]��һ��]\n");
        return BAD_REQUEST;
    }
    LOG_INFO("�����н�����ϣ�NO_REQUEST\n");
    m_checked_state = CHECK_STATE_HEADER;//״̬��Ϊ��Ҫ��ȡͷ����
    return NO_REQUEST;
}

//����HTTP�����һ��ͷ����Ϣ
http_conn::HTTP_CODE http_conn::parse_headers(char* text)
{

    printf("����ͷ����");
    if (text[0] == '\0')
    {
        printf("textʣ�����ݣ�%s", text + 1);
        //���HTTP��������Ϣ�壬����Ҫ��ȡm_content_length�ֽڵ���Ϣ�壬״̬��ת�Ƶ�CHECK_STATE_CONTENT״̬
        if (m_content_length != 0)
        {
            printf("�������ˣ���������");
            m_checked_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        //����˵�������Ѿ��õ���һ��������HTTP����
        printf("����˵�������Ѿ��õ���һ��������HTTP����");
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        printf("------------------------------conn----------------/n");
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    //����Content-lengthͷ���ֶ�
    else if (strncasecmp(text, "Content-Length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
        printf("----------------------------------------------/n");
        printf("/n/n strncasecmp:[%d]/n/n", m_content_length);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        printf("-----------------------------------host-----------/n");
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        printf("---------------------------oop-------------------/n");
        LOG_INFO("oop! unknow header: %s\n", text);
    }
    return NO_REQUEST;
}

//�ж����Ƿ������Ķ�����
http_conn::HTTP_CODE http_conn::parse_content(char* text)
{
    printf("panduan-----");
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        printf("�����е�����Ϊ��&s", text);
        text[m_content_length] = '\0';
        m_resquest_data = text;//��ȡʣ�µ�������
        printf("�����е�����Ϊ��&s", text);
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//��״̬��process 1 retrun
http_conn::HTTP_CODE http_conn::process_read()
{

    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;
    //
    while (((m_checked_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) || ((line_status = parse_line()) == LINE_OK))
    {

        text = get_line();
        m_start_line = m_checked_idx;//������Ϊǰ�������parse_line���¼������Ѿ�������
        LOG_INFO("process_read():got 1 http line:%s\n", text);
        printf("process_read():got 1 http line:%s\n", text);
        switch (m_checked_state)
        {
        case CHECK_STATE_REQUESTION:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
            {
                LOG_ERROR("parse_request_line BAD_REQUEST\n");
                return BAD_REQUEST;
            }
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
            {
                LOG_ERROR("parse_headers BAD_REQUEST\n");
                return BAD_REQUEST;
            }
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            printf("case CHECK_STATE_CONTENT:");
            ret = parse_content(text);
            if (ret == GET_REQUEST)
            {
                return do_request();
            }
            line_status = LINE_OPEN;
            break;
        }


        default:
        {
            return INTERNAL_ERROR;
        }
        }
    }
    LOG_DEBUG("process_read return\n");
    return NO_REQUEST;
}

//���õ�һ�������ġ���ȷ��HTTP����ʱ�����Ǿͷ���Ŀ���ļ������ԡ����Ŀ���ļ����ڣ��������û��ɶ����Ҳ���Ŀ¼����ʹ��mmap����ӳ�䵽�ڴ��ַm_file_address���������ߵ������ļ���ȡ�ɹ�
http_conn::HTTP_CODE http_conn::do_request()
{
    LOG_DEBUG("do_reaquest!\n");
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);

    const char* p = strrchr(m_url, '/');

    //�����¼����load or register
    if (m_method == POST && (*(p + 1) == 'l' || *(p + 1) == 'r'))
    {
        printf("�����¼����load\n");
        char name[100], password[100];
        int i = 0;
        //��ȡ�������е��û���������  users=123&password=xxx

        printf("��ȡ�������е��û���������\n");

        for (i = 5; m_resquest_data[i] != '&'; ++i)
        {
            name[i - 5] = m_resquest_data[i];
        }
        printf("����name:[%s]\n", name);
        name[i - 5] = '\0';
        int j = 0;
        for (i = i + 10; m_resquest_data[i] != '\0'; ++i, ++j)
        {
            password[j] = m_resquest_data[i];
        }
        password[j] = '\0';
        printf("����passwd\n");
        //load
        printf("��¼\n");
        if (*(p + 1) == 'l')
        {
            if (m_users.find(name) != m_users.end() && m_users[name] == password)
            {
                strcpy(m_url, "/index.html");
            }
            else
            {
                strcpy(m_url, "/logerror.html");
            }
        }
        else
        {
            printf("ע��\n");
            //�����ע�ᣬ�ȼ�����ݿ����Ƿ���������
            //û�������ģ�������������
            printf("name:[&s]\n", name);
            printf("password:[&s]\n", password);
            printf("������ing\n");
            char* sql_insert = (char*)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");
            printf("ע��˶�\n");
            if (m_users.find(name) == m_users.end())
            {
                m_lock.lock();
                printf("������ing\n");
                printf("������ing:[%s]\n", sql_insert);



                int res = mysql_query(mysql, sql_insert);
                printf("������ɹ�\n");
                m_users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/load.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }

    }


    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    /*
    ͷ�ļ���#include <sys/stat.h>   #include <unistd.h>
    ���庯����int stat(const char * file_name, struct stat *buf);
    ����˵����stat()����������file_name ��ָ���ļ�״̬, ���Ƶ�����buf ��ָ�Ľṹ�С�
    */
    LOG_INFO("do_request()�����ļ�����%s", m_real_file);
    if (stat(m_real_file, &m_file_stat) < 0)
    {
        LOG_ERROR("�ļ������ڣ�\n");
        return NO_REQUEST;
    }
    if (!(m_file_stat.st_mode & S_IROTH))
    {
        LOG_ERROR("�ļ����ɶ���\n");
        return FORBIDDEN_REQUEST;
    }
    if (S_ISDIR(m_file_stat.st_mode))
    {

        LOG_ERROR("�ļ���Ŀ¼��\n");
        return BAD_REQUEST;
    }
    int fd = open(m_real_file, O_RDONLY);
    /*
    #inlcude<sys/mann.h>
    void mmap(void *start, size_t length, int prot, int flags, int fd, off_t offset);
    int munmap(void *start, size_t length);

    void *start �����û�ʹ��ĳһ�����ض��ĵ�ַΪ������ڴ����ʼλ�á������������ΪNULL����ϵͳ�Զ�����һ����ַ��
    size_t length �˲����ƶ����ڴ�εĳ���
    int prot �˲��������ڴ�η���Ȩ�ޣ�
            PROT_READ:�ɶ�
            PROT_WRITE:��д
            PROT_EXEC:��ִ��
            PROT_NONE:�ڴ�β��ܱ�����
    int flags �˲��������ڴ�����ݱ��޸ĺ�������Ϊ�������Ա�����Ϊ����ֵ�İ�λ��MAP_SHARED��MAP_PRIVATE�ǻ���ģ�����ͬʱָ����
            MAP_SHARED:�ڽ��̼乲������ڴ档�Ը��ڴ�ε��޸Ľ���Ӧ����ӳ����ļ��С����ṩ�˽��̼乲���ڴ��POSIX����
            MAP_PRIVATE:�ڴ�ε���Ϊ����˽�У��Ը��ڴ�ε��޸Ĳ��ᷴӦ����ӳ����ļ���
            MAP_ANONYMOUS:����ڴ治�Ǵ��ļ�ӳ������ģ������ݱ���ʼ��Ϊȫ0����������£�mmap�������������������������
            MAP_FIXED:�ڴ�α���λ��start����ָ���ĵ�ַ����start�������ڴ�ҳ���С��4096����������
            MAP_HUGETLB:���մ��ڴ�ҳ���������ڴ�ռ䡣���ڴ�ҳ��Ĵ�С����ͨ��/pro/meminfo�ļ����鿴
    int fd �˲����Ǳ�ӳ���ļ���Ӧ���ļ�����������һ��ͨ��openϵͳ���û�á�
    off_t offset�˲������ô��ļ��ĺδ���ʼӳ�䣨���ڲ���Ҫ���������ļ��������
    mmap�����ɹ�ʱ����ָ��Ŀ���ڴ������ָ�룬ʧ���򷵻�MAO_FAILED((void*)-1)������errno
    munmap�����ɹ�����0.ʧ�ܷ���-1������errno

    */
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);//ӳ�乲���ڴ�
    close(fd);
    return FILE_REQUEST;

}

//��Ϊ���溯�������Ѿ����򿪵��ļ�ӳ�䵽���ڴ棬���Ա�����ڴ�ӳ����ִ��munmap����
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

//дhttp��Ӧ
bool http_conn::write()
{

    int temp = 0;
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }
    while (1)
    {
        // printf("wirte() ready!\n");
        // temp = writev(m_sockfd, m_iv, m_iv_count);
        // printf("wirte() over!\n");
        // if(temp <= -1)
        // {
        //     //���TCPд����û�пռ䣬��ȴ���һ��RPOLLOUT�¼�����Ȼ�ڴ��ڼ䣬�������޷��������ܵ�ͬһ���ͻ�����һ�����󣬵�����Ա�֤���ӵ������ԡ�
        //     if(errno == EAGAIN)
        //     {
        //         printf("wirte() file for space lack!\n");
        //         modfd(m_epollfd, m_sockfd, EPOLLOUT);
        //         return true;
        //     }
        //     unmap();
        //     return false;
        // }

        // printf("wirte() fished!\n");
        // bytes_to_send -= temp;
        // bytes_have_send += temp;
        // if(bytes_to_send <= bytes_have_send)
        // {
        //     //����HTTP��Ӧ�ɹ�������HTTP�����е�Connection�ֶξ����Ƿ������ر�����
        //     unmap();
        //     if(m_linger)
        //     {
        //         printf("�����ӱ��ּ�����\n");
        //         init();
        //         modfd(m_epollfd, m_sockfd, EPOLLIN);
        //         return true;
        //     }
        //     else
        //     {
        //         printf("�����ӹرռ�����\n");
        //         modfd(m_epollfd, m_sockfd, EPOLLIN);
        //         return false;
        //     }
        // }

        //����Ϊ�˽�����ļ��������� bycdy 20210708
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                LOG_ERROR("[EAGAIN]---------write() temp < 0--------\n");
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }
        //���˸�temp�ֽ������ļ�

        bytes_have_send += temp;
        //�ѷ���temp�ֽ������ļ�

        bytes_to_send -= temp;
        //������Է��͵��ֽڴ��ڱ�ͷ��֤����ͷ������ϣ��������ļ�Ҫ����
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            //��ͷ��������
            m_iv[0].iov_len = 0;
            /*���д��룺��Ϊm_write_idx��ʾΪ�������ļ��Ķ�λ�㣬m_iv[0]ָ��m_write_buf��
            ����bytes_have_send���ѷ��͵��������� - m_write_idx���ѷ�����ı�ͷ�е���������
            �͵���ʣ�෢���ļ�ӳ��������ʼλ��*/
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        //����������ͱ�ͷ���޸�m_ivָ��д��������λ���Լ������͵ĳ����Ա��´ν��ŷ�
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            //������ϣ��ָ�Ĭ��ֵ�Ա��´μ��������ļ�
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);
            LOG_INFO("[�ļ��������]---------bytes_to_send < 0--------\n");
            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

//��������д������͵�����,Ҳ����д����m_write_buf
bool http_conn::add_response(const char* format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
    {
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format);//���տɱ����
    int len = vsnprintf(m_write_buf + m_write_idx,
        WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);//���ɱ����д����һ���������ڴ��д�ĳ���Ϊ�ڶ�������

    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);//�ͷſɱ������Դ
    return true;
}

bool http_conn::add_status_line(int status, const char* title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}


//Ӧ���е�ͷ���ֶ�
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() && add_blank_line();
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char* content)
{
    return add_response("%s", content);
}

//���ݷ���������HTTP����Ľ�����������ظ��ͻ��˵�����
bool http_conn::process_write(HTTP_CODE ret)
{

    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
        {
            return false;
        }
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(400, error_400_title);
        add_headers(strlen(error_400_form));
        if (!add_content(error_400_form))
        {
            return false;
        }
        break;
    }
    case NO_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
        {
            return false;
        }
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
        {
            return false;
        }
        break;
    }
    case FILE_REQUEST://�����ļ�
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            LOG_INFO("���͵ı�ͷΪ��\n%s\n", m_write_buf);
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;//���贫��������ֽ�
            return true;
        }
        else
        {
            const char* ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
            {
                return false;
            }
        }
        break;
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    LOG_INFO("���͵ı�ͷΪ��\n%s\n", m_write_buf);
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}


//���̳߳صĹ������ã����Ǵ���HTTP�������ں���
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        //printf("process_read ret == NO_REQUEST! \n");
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    //printf("process_write ready!");
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        LOG_ERROR("process_write fail, fd ==[%d] closed \n", m_sockfd);
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}
