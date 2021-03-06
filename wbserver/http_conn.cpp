#include <mysql/mysql.h>
#include "http_conn.h"
#include <fstream>

//定义HTTP响应的状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "you do not have permission to get file form this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";



//网站的根目录
const char* doc_root = "/home/chendongyu/root";

//map<string, string> users;
locker m_lock;

//数据库相关
void http_conn::initmysql_result(connection_pool* connPool)
{
    //先从连接池中取一个连接
    printf("先从连接池中取一个连接\n");
    MYSQL* mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据
    printf("在user表中检索username，passwd数据\n");
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    printf("从表中检索完整的结果集\n");
    MYSQL_RES* result = mysql_store_result(mysql);

    //返回结果集中的列数
    printf("返回结果集中的列数\n");
    if (result == NULL) printf("result wei kong\n");
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    printf("返回所有字段结构的数组\n");
    MYSQL_FIELD* fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    printf("从结果集中获取下一行，将对应的用户名和密码，存入map中\n");
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        m_users[temp1] = temp2;
    }
}







//设置为非阻塞文件描述符
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//上树
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
        event.events = EPOLLIN | EPOLLRDHUP;//EPOLLRDHUP对端断开链接
    }

    if (one_shot)
    {
        event.events |= EPOLLONESHOT;
    }
    LOG_INFO("addfd！fd:[%d]\n", fd);
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从树上移除文件描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//修改树上的文件描述符
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

//关闭连接
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

//对象初始化
void http_conn::init(int sockfd, const sockaddr_in& addr, int trig_model, sort_timer_lst* timer_list)
{

    m_timer_list = timer_list;

    m_sockfd = sockfd;
    m_address = addr;
    char _ip[64];
    inet_ntop(AF_INET, &m_address.sin_addr, _ip, sizeof(_ip));
    LOG_INFO("接入 ip = [%s] fd == [%d]\n", _ip, sockfd);

    m_trig_model = trig_model;
    //如下两行是为了避免TIME_WAIT状态，仅用于调试，实际使用时应该去掉,也就是端口复用
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


//从状态机，用于解析出一行内容
http_conn::LINE_STATUS http_conn::parse_line()
{

    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        //获得当前分析的字
        temp = m_read_buf[m_checked_idx];
        //如果当前的字节是\r，即回车符，则说明可能读取到一个完整的行
        if (temp == '\r')
        {
            //如果\r字符碰巧是目前buffer中的最后一个已经被读入的客户数据，那么这次分析没有读取到一个完整的行，需要继续读取数据
            if (m_checked_idx + 1 == m_read_idx)
            {
                return LINE_OPEN;
            }

            //表示读到了一个完整的行
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {

                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            LOG_DEBUG("pares_line()语法错误\n");
            //以上都不是，就是存在语法错误
            return LINE_BAD;

        }
        //当前字符为\n也有可能是到了一行的情况
        else if (temp == '\n')
        {
            //因为\r\n一起用，还得判断
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
    //如果所有的字符都读完了还没有遇到
    return LINE_OPEN;

}

//循环读取客户端数据，知道无数据可读或者对方关闭链接
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
            if (errno == EAGAIN || errno == EWOULDBLOCK)//recv返回EWOULDBLOCK表示缓冲无数据
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

//解析HTTP请求行，获取请求方法，目标URL以及HTTP版本号
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
        LOG_ERROR("BAD_REQUEST[m_url和m_url[0]不一致]\n");
        return BAD_REQUEST;
    }
    LOG_INFO("请求行解析完毕，NO_REQUEST\n");
    m_checked_state = CHECK_STATE_HEADER;//状态改为需要读取头部行
    return NO_REQUEST;
}

//解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text)
{

    printf("解析头部行");
    if (text[0] == '\0')
    {
        printf("text剩余内容：%s", text + 1);
        //如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，状态机转移到CHECK_STATE_CONTENT状态
        if (m_content_length != 0)
        {
            printf("解析完了，还有内容");
            m_checked_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        //否则说明我们已经得到了一个完整的HTTP请求
        printf("否则说明我们已经得到了一个完整的HTTP请求");
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
    //处理Content-length头部字段
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

//判断他是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content(char* text)
{
    printf("panduan-----");
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        printf("内容行的内容为：&s", text);
        text[m_content_length] = '\0';
        m_resquest_data = text;//读取剩下的内容行
        printf("内容行的内容为：&s", text);
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//主状态机process 1 retrun
http_conn::HTTP_CODE http_conn::process_read()
{

    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;
    //
    while (((m_checked_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) || ((line_status = parse_line()) == LINE_OK))
    {

        text = get_line();
        m_start_line = m_checked_idx;//这里因为前面调用了parse_line导致计数器已经更新了
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

//当得到一个完整的、正确的HTTP请求时，我们就分析目标文件的属性。如果目标文件存在，对所有用户可读，且不是目录，则使用mmap将其映射到内存地址m_file_address处，并告诉调用者文件获取成功
http_conn::HTTP_CODE http_conn::do_request()
{
    LOG_DEBUG("do_reaquest!\n");
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);

    const char* p = strrchr(m_url, '/');

    //处理登录界面load or register
    if (m_method == POST && (*(p + 1) == 'l' || *(p + 1) == 'r'))
    {
        printf("处理登录界面load\n");
        char name[100], password[100];
        int i = 0;
        //读取内容行中的用户名和密码  users=123&password=xxx

        printf("读取内容行中的用户名和密码\n");

        for (i = 5; m_resquest_data[i] != '&'; ++i)
        {
            name[i - 5] = m_resquest_data[i];
        }
        printf("读完name:[%s]\n", name);
        name[i - 5] = '\0';
        int j = 0;
        for (i = i + 10; m_resquest_data[i] != '\0'; ++i, ++j)
        {
            password[j] = m_resquest_data[i];
        }
        password[j] = '\0';
        printf("读完passwd\n");
        //load
        printf("登录\n");
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
            printf("注册\n");
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            printf("name:[&s]\n", name);
            printf("password:[&s]\n", password);
            printf("语句插入ing\n");
            char* sql_insert = (char*)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");
            printf("注册核对\n");
            if (m_users.find(name) == m_users.end())
            {
                m_lock.lock();
                printf("语句插入ing\n");
                printf("语句插入ing:[%s]\n", sql_insert);



                int res = mysql_query(mysql, sql_insert);
                printf("语句插入成功\n");
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
    头文件：#include <sys/stat.h>   #include <unistd.h>
    定义函数：int stat(const char * file_name, struct stat *buf);
    函数说明：stat()用来将参数file_name 所指的文件状态, 复制到参数buf 所指的结构中。
    */
    LOG_INFO("do_request()请求文件名：%s", m_real_file);
    if (stat(m_real_file, &m_file_stat) < 0)
    {
        LOG_ERROR("文件不存在！\n");
        return NO_REQUEST;
    }
    if (!(m_file_stat.st_mode & S_IROTH))
    {
        LOG_ERROR("文件不可读！\n");
        return FORBIDDEN_REQUEST;
    }
    if (S_ISDIR(m_file_stat.st_mode))
    {

        LOG_ERROR("文件是目录！\n");
        return BAD_REQUEST;
    }
    int fd = open(m_real_file, O_RDONLY);
    /*
    #inlcude<sys/mann.h>
    void mmap(void *start, size_t length, int prot, int flags, int fd, off_t offset);
    int munmap(void *start, size_t length);

    void *start 允许用户使用某一个人特定的地址为有这段内存的起始位置。如果他被设置为NULL，则系统自动分配一个地址。
    size_t length 此参数制定了内存段的长度
    int prot 此参数设置内存段访问权限：
            PROT_READ:可读
            PROT_WRITE:可写
            PROT_EXEC:可执行
            PROT_NONE:内存段不能被访问
    int flags 此参数控制内存段内容被修改后程序的行为。它可以被设置为以下值的按位或（MAP_SHARED和MAP_PRIVATE是互斥的，不能同时指定）
            MAP_SHARED:在进程间共享这段内存。对该内存段的修改将反应到被映射的文件中。它提供了进程间共享内存的POSIX方法
            MAP_PRIVATE:内存段调用为进程私有，对该内存段的修改不会反应到被映射的文件中
            MAP_ANONYMOUS:这段内存不是从文件映射而来的，其内容被初始化为全0，这种情况下，mmap函数的最后两个参数将被忽略
            MAP_FIXED:内存段必须位于start参数指定的地址处。start必须是内存页面大小（4096）的整数倍
            MAP_HUGETLB:按照大内存页面来分配内存空间。大内存页面的大小可以通过/pro/meminfo文件来查看
    int fd 此参数是被映射文件对应的文件描述符。他一般通过open系统调用获得。
    off_t offset此参数设置从文件的何处开始映射（对于不需要读入整个文件的情况）
    mmap函数成功时返回指向目标内存区域的指针，失败则返回MAO_FAILED((void*)-1)并设置errno
    munmap函数成功返回0.失败返回-1并设置errno

    */
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);//映射共享内存
    close(fd);
    return FILE_REQUEST;

}

//因为上面函数我们已经将打开的文件映射到了内存，所以必须对内存映射区执行munmap操作
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

//写http响应
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
        //     //如果TCP写缓冲没有空间，则等待下一轮RPOLLOUT事件。虽然在此期间，服务器无法立即接受到同一个客户的下一个请求，但这可以保证链接的完整性。
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
        //     //发送HTTP相应成功，更具HTTP请求中的Connection字段决定是否立即关闭连接
        //     unmap();
        //     if(m_linger)
        //     {
        //         printf("长链接保持监听！\n");
        //         init();
        //         modfd(m_epollfd, m_sockfd, EPOLLIN);
        //         return true;
        //     }
        //     else
        //     {
        //         printf("短链接关闭监听！\n");
        //         modfd(m_epollfd, m_sockfd, EPOLLIN);
        //         return false;
        //     }
        // }

        //以下为了解决大文件传输问题 bycdy 20210708
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
        //读了个temp字节数的文件

        bytes_have_send += temp;
        //已发送temp字节数的文件

        bytes_to_send -= temp;
        //如果可以发送的字节大于报头，证明报头发送完毕，但还有文件要发送
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            //报头长度清零
            m_iv[0].iov_len = 0;
            /*这行代码：因为m_write_idx表示为待发送文件的定位点，m_iv[0]指向m_write_buf，
            所以bytes_have_send（已发送的数据量） - m_write_idx（已发送完的报头中的数据量）
            就等于剩余发送文件映射区的起始位置*/
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        //否则继续发送报头，修改m_iv指向写缓冲区的位置以及待发送的长度以便下次接着发
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            //发送完毕，恢复默认值以便下次继续传输文件
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);
            LOG_INFO("[文件发送完毕]---------bytes_to_send < 0--------\n");
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

//往缓冲区写入待发送的数据,也就是写入了m_write_buf
bool http_conn::add_response(const char* format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
    {
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format);//接收可变参数
    int len = vsnprintf(m_write_buf + m_write_idx,
        WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);//将可变参数写到第一个参数的内存里，写的长度为第二个参数

    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);//释放可变参数资源
    return true;
}

bool http_conn::add_status_line(int status, const char* title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}


//应答行的头部字段
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

//根据服务器处理HTTP请求的结果，决定返回给客户端的内容
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
    case FILE_REQUEST://请求文件
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            LOG_INFO("发送的报头为：\n%s\n", m_write_buf);
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;//还需传入的数据字节
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
    LOG_INFO("发送的报头为：\n%s\n", m_write_buf);
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}


//由线程池的工作调用，这是处理HTTP请求的入口函数
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
