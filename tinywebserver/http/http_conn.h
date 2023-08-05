#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <csignal>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cassert>
#include <sys/stat.h>
#include <cstring>
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <cstdarg>
#include <cerrno>
#include <sys/wait.h>
#include <sys/uio.h>

#include "../log/log.h"
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"/

class http_conn{
public:
    static const int FILENAME_LEN = 200;       // 请求资源的最大长度
    static const int READ_BUFFER_SIZE = 2048;  // 2k读缓冲区
    static const int WRITE_BUFFER_SIZE = 1024; // 读缓冲区

    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATH};  // HTTP请求方法
    enum CHECK_STATE{
        // 主状态机
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    enum HTTP_CODE{
        // 解析结果
        NO_REQUEST = 0,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };

    enum LINE_STATUS{
        // 从状态机
        LINE_OK = 0, 
        LINE_BAD,
        LINE_OPEN
    };

public:
    http_conn(){}
    ~http_conn(){}

public:
    void init(int sockfd, const sockaddr_in& caddr);
    void close_conn(bool real_close = true);
    void process();
    bool read_once();
    bool write();
    sockaddr_in* get_address(){
        return &m_address;
    }
    void initmysql_result(connection_pool* connPool);

private:
    void init();
    HTTP_CODE process_read();
    bool process_write(HTTP_CODE ret);
    HTTP_CODE parse_request_line(char* text);
    HTTP_CODE parse_headers(char* text);
    HTTP_CODE parse_content(char* text);
    HTTP_CODE do_request();
    char* getline(){ return m_read_buf + m_start_line; }
    LINE_STATUS parse_line();
    void unmap();
    bool add_response(const char* format, ...);
    bool add_content(const char* content);
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL *mysql;  //

private:
    // 用户信息
    int m_sockfd;
    sockaddr_in m_address;
    // 读缓冲区相关参数
    char m_read_buf[READ_BUFFER_SIZE];
    int m_read_idx;                      // 记录下一个可读位置索引
    int m_checked_idx;                  // http请求报文解析过程中，下一个可判断位置索引
    int m_start_line;                  // 记录当前解析行首的偏移量
    // 写缓冲区相关参数
    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_write_idx;                 // 用户区写缓冲区下一个可写位置索引
    // http解析相关参数
    CHECK_STATE m_check_state;
    // 解析请求行的参数
    METHOD m_method;
    char* m_url;
    char* m_version;
    // 解析请求头的参数
    char* m_host;
    int m_content_length;
    bool m_linger;
    // 请求资源相关参数
    char m_real_file[FILENAME_LEN];  // 用户保存请求资源的路径
    char* m_file_address;
    // 分散写
    struct stat m_file_stat;
    struct iovec m_iv[2];
    int m_iv_count;
    int cgi;    // 是否启用的POST
    char* m_string;  // 存储请求头数据
    int bytes_to_send;
    int bytes_have_send;
};
#endif