#include"http_conn.h"
#include<map>
#include<mysql/mysql.h>
#include<fstream>

#define connfdET // 边缘触发非阻塞
// #define connfdLT // 水平触发阻塞

// #define listenfdET // 边缘触发非阻塞
// #define listenfdLT // 水平触发阻塞

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

// 当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问文件中内容为空
const char* doc_root = "/home/xxxx/tinywebserver/root";   // 设置自己的资源路径

// 将表中的用户名和密码放入到map
std::map<std::string, std::string> users;
locker m_lock;

// 数据库初始化，导入表中用于信息
void http_conn::initmysql_result(connection_pool* connPool){
    // 从sql连接池中取出一个连接
    MYSQL* mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    // 从user表中检索username, password数据，浏览器端输入j 日志记录错误信息
    if(mysql_query(mysql, "SELECT username, passwd FROM user")){
        LOG_ERROR("SELECT error:%s\n", mysql_errno(mysql)); 
    }

    // 从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    // 返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    // 返回所有字段结构的数组
    MYSQL_FIELD* fields = mysql_fetch_fields(result);

    // 从结果集中获取下一行，将对应的用户名和密码，存入maop
    while(MYSQL_ROW row = mysql_fetch_row(result)){
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

// 将文件描述符设置为非阻塞
int setnonblocking(int fd){
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT；此版本有BUG
// void addfd(int epollfd, int fd, bool one_shot){
//     epoll_event ev;
//     ev.data.fd = fd;

// #ifdef connfdET
//     ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
// #endif

// #ifdef connfdLT
//     ev.events = EPOLLIN | EPOLLRDHUP;
// #endif

// #ifdef listenfdET
//     ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
// #endif

// #ifdef listenfdLT
//     ev.events = EPOLLIN | EPOLLRDHUP;
// #endif

//     if(one_shot){
//         ev.events |= EPOLLONESHOT;
//     }

//     epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
//     setnonblocking(fd);            // 设置文件描述符为非阻塞的
// }

// 新版本
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode){
    epoll_event ev;
    ev.data.fd = fd;

    if(1 == TRIGMode){
        ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
    }else{
        ev.events = EPOLLIN | EPOLLRDHUP;
    }

    if(one_shot){
        ev.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
    setnonblocking(fd);            // 设置文件描述符为非阻塞的
}

// 从内核事件表中删除描述符
void removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
}

// 将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int event){
    epoll_event ev;
    ev.data.fd = fd;

#ifdef connfdET
    ev.events = event | EPOLLRDHUP | EPOLLET | EPOLLONESHOT;
#endif

#ifdef connfdLT
    ev.events = event | EPOLLONESHOT | EPOLLRDHUP;
#endif

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &ev);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

// 关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close){
    if(real_close && (m_sockfd != -1)){
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        --m_user_count;
    }
}

// 初始化连接, 外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in& caddr){
    m_sockfd = sockfd;
    m_address = caddr;
    // int reuse = 1;
    // setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef connfdET
    addfd(m_epollfd, m_sockfd, true, 1);  
#endif

#ifdef connfdLT
    addfd(m_epollfd, m_sockfd, true, 0);
#endif
    m_user_count++;
    init();
}

// 初始化新接受的连接
// check_state默认从请求行开始
void http_conn::init(){
    mysql = NULL;
    memset(m_read_buf, '\0', sizeof(m_read_buf));
    m_read_idx = 0;                          // 下一个读开始的起始下标
    m_checked_idx = 0;
    m_start_line = 0;
    memset(m_write_buf, '\0', sizeof(m_write_buf));
    m_write_idx = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_method = GET;
    memset(m_real_file, '\0', sizeof(m_real_file));
    m_url = NULL;
    m_version = NULL;
    m_host = NULL;
    m_content_length = 0;
    m_linger = false;
    cgi = 0;
    bytes_to_send = 0;
    bytes_have_send = 0;
   
}

// 从状态机，用于分析出一行内容
// 返回值为行的读取状态，有LINE_OK, LINE_BAD, LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line(){
    char temp;
    for( ; m_checked_idx < m_read_idx; ++m_checked_idx){
        temp = m_read_buf[m_checked_idx];
        if(temp == '\r'){
            if(m_read_idx == (m_checked_idx + 1)){
                return LINE_OPEN;
            }else if(m_read_buf[m_checked_idx + 1] == '\n'){
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }

        if(temp == '\n'){ // 为什么大于1, 使得LINE_OPEN条件下有效
            if(m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r'){
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}


//循环读取客户数据，直到无数据可读或对方关闭连接
// 在非阻塞ET工作模式下，需要一次洗将数据读完
bool http_conn::read_once(){
    if(m_read_idx >= READ_BUFFER_SIZE){
        return false;
    }
    int bytes_read = 0;

#ifdef connfdLT
    bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
    m_read_idx += bytes_read;
    if(bytes_read <= 0 ){
        return false;
    }
    return true;
#endif

#ifdef connfdET
    // ET模型需要循环读
    while(true){
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if(bytes_read == -1){
            if(errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return false;
        }
        else if(bytes_read == 0){  // 对方关闭连接
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
#endif
}

// 解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text){
    if(text == NULL){return BAD_REQUEST;}
    m_url = strpbrk(text, " \t"); // 匹配字符集中的第一个, 请求行中有的以span或者\t间隔
    if(m_url == NULL){return BAD_REQUEST;}
    *m_url++ = '\0';
    if(strcasecmp(text, "GET") == 0){
        m_method = GET;
    }else if(strcasecmp(text, "POST") == 0){
        m_method = POST;
        cgi = 1;    // 标志位
    }else{
        return BAD_REQUEST;
    }

    // 解析m_version
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if(NULL == m_version){return BAD_REQUEST;}
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    // if (strcasecmp(m_version, "HTTP/1.1") != 0){return BAD_REQUEST;}
    
    // 解析m_url
    if(strncasecmp(m_url, "http://", 7) == 0){
        m_url += 7;
        m_url = strchr(m_url, '/');  // 定义到请求资源
    }

    if(strncasecmp(m_url, "https://", 8) == 0){
        m_url += 8;
        m_url = strchr(m_url, '/');  // 定义到请求资源
    }

    if(!m_url || m_url[0] != '/'){return BAD_REQUEST;}
    // 当url为/时，显示判断界面
    if(strlen(m_url) == 1){
        strcat(m_url, "judge.html");
    }
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text){
    if(NULL == text){return BAD_REQUEST;}

    // 解析到空行了
    if(text[0] == '\0' && text[1] == '\0'){ 
        if(m_content_length == 0){
            return GET_REQUEST;
        }
        m_check_state = CHECK_STATE_CONTENT;
    }else if(strncasecmp(text, "Connection:", 11) == 0){
        text += 11;
        text += strspn(text, " \t");
        if(strcasecmp(text, "keep-alive") == 0){
            m_linger = true;
        }
    }else if(strncasecmp(text, "Content-length:", 15) == 0){
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }else if(strncasecmp(text, "Host:", 5) == 0){
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }else{
        // printf("oop! unknown header: %s\n", text);
        LOG_INFO("oop! unknow header: %s", text);
        Log::get_instance()->flush(); // 还有个日志函数
    }
    return NO_REQUEST;
}

// http请求体并未解析
//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// http请求解析，利用主从有限状态机运行
http_conn::HTTP_CODE http_conn::process_read(){
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;
    while((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK)){
        text = getline();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);               // 记录日志
        Log::get_instance()->flush();      // 刷新缓冲区
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
            ret = parse_request_line(text);
            if(ret == BAD_REQUEST){return BAD_REQUEST;}
            break;
        case CHECK_STATE_HEADER:
            ret = parse_headers(text);
            if(ret == BAD_REQUEST){return BAD_REQUEST;}
            else if(ret == GET_REQUEST){
                return do_request();
            }
            break;
        case CHECK_STATE_CONTENT:
            ret = parse_content(text);
            if(ret == GET_REQUEST){return do_request();}
            line_status = LINE_OPEN;
            break;
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

// 在语法正确的情况下解析http请求
http_conn::HTTP_CODE http_conn::do_request(){

    // 首先是url拼接，这是根路径
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    // printf("m_url:%s\n", m_url);
    const char* p = strchr(m_url, '/');
    // POST http://192.168.100.100:9999/2CGISQL.cgi
    // 处理cgi, *(p + 1) == '2'这是什么意思, '2':登录， '3':注册
    if(cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3')){
        // 根据标志位判断是登录检测还是注册检测
        char flag = m_url[1];

        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len -1);
        free(m_url_real);

        // 将用户名和密码提取出来
        //user=123&password=123
        char name[100], password[100];
        int i;
        for(i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];  // 用户名
        name[i - 5] = '\0';
        
        int j = 0;
        for(i = i + 10; m_string[i] != '\0'; ++i, ++j){
            password[j] = m_string[i];
        }
        password[j] = '\0';

        // 同步线程登录校验
        if(*(p + 1) == '3'){
            // 如果是注册，先检测数据库中是否有重名
            // 没有重名则进行增加数据
            char* sql_insert = (char*) malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')'");

            if(users.find(name) == users.end()){
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(std::make_pair<string, string>(name, password));
                m_lock.unlock();

                if(!res){
                    strcpy(m_url, "/log.html");
                }else{
                    strcpy(m_url, "/registerError.html");
                }
            }else{
                strcpy(m_url, "/registerError.html");
            }
        }

        // 如果是登录就直接判断
        // 若浏览器端输入的用户名和密码在表中口语查找到，返回1，否则返回0
        else if(*(p + 1) == '2'){
            if(users.find(name) != users.end() && users[name] == password){
                strcpy(m_url, "/welcome.html");
            }else{
                strcpy(m_url, "/logError.html");
            }
        }
    }

    // 对各种情况下的url解析
    if(*(p + 1) == '0'){
        char *m_url_real = (char*) malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }else if(*(p + 1) == '1'){
        char *m_url_real = (char*) malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }else if(* (p + 1) == '5'){
        char* m_url_real = (char*) malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }else if(*(p + 1) == '6'){
        char* m_url_real = (char*) malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }else if(*(p + 1) == '7'){
        char* m_url_real = (char*) malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }else{
        strncpy(m_real_file + len, m_url, FILENAME_LEN - 1 - len);
    }

    // 对解析的url进行条件判断
    if(stat(m_real_file, &m_file_stat) < 0){
        return NO_RESOURCE;
    }
    // 判断是否有权限
    if(!(m_file_stat.st_mode & S_IROTH)){
        return FORBIDDEN_REQUEST;
    }
    // 判断是否是文件
    if(S_ISDIR(m_file_stat.st_mode)){
        return BAD_REQUEST;
    }
    // 走到这一步，基本判断是有效的文件请求了
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char*) mmap(NULL, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

// 释放存储映射
void http_conn::unmap(){
    if(m_file_address){
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = NULL;
    }
}

// 写数据,将准备好的数据写入到sockfd中
bool http_conn::write(){
    int temp = 0;
    if(bytes_to_send == 0){
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while(true){
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if(temp < 0){
            if(errno == EAGAIN){
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if(bytes_have_send >= m_iv[0].iov_len){
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }else{
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_write_idx - bytes_have_send;
        }

        if(bytes_to_send <= 0){
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);
            if(m_linger){
                init();
                return true;
            }else{
                return false;
            }
        }
    }
}

// 生成响应的格式
bool http_conn::add_response(const char* format, ...){
    if(m_write_idx >= WRITE_BUFFER_SIZE){
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if(len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)){
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    LOG_INFO("request:%s", m_write_buf);
    Log::get_instance()->flush();
    return true;
}

 bool http_conn::add_status_line(int status, const char* title){
     return add_response("HTTP/1.1 %d %s\r\n", status, title);
 }

bool http_conn::add_headers(int content_len){
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len){
    return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_content_type(){
    return add_response("Content-Type: %s\r\n", "text/html");
}

bool http_conn::add_linger(){
    return add_response("Connection: %s\r\n", (m_linger == true)?"keep-alive" : "close");
}

bool http_conn::add_blank_line(){
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char* content){
    return add_response("%s", content);
}

// 工作线程准备好数据？
bool http_conn::process_write(HTTP_CODE ret){
    switch(ret){
        case INTERNAL_ERROR:
        {
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form))
                return false;
            break;
        }
        case BAD_REQUEST:
        {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if(!add_content(error_404_form))
                return false;
            break;
        }
        case FORBIDDEN_REQUEST:
        {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if(!add_content(error_403_form))
                return false;
            break;
        }
        case FILE_REQUEST:
        {
            add_status_line(200, ok_200_title);
            if(m_file_stat.st_size != 0){
                add_headers(m_file_stat.st_size);
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }else{
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
        }
        default:
            return false;
    }

    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

void http_conn::process(){
    HTTP_CODE read_ret = process_read();
    if(read_ret == NO_REQUEST){
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    bool write_ret = process_write(read_ret);
    if(!write_ret){
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}