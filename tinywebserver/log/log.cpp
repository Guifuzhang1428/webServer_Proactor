#include<cstring>
#include<time.h>
#include<sys/time.h>
#include<stdarg.h>
#include"log.h"
#include<pthread.h>

Log::Log(){
    m_count = 0;
    m_is_asyn = false;
}

Log::~Log(){
    if(m_fp != NULL){
        fclose(m_fp);
    }
}

// 异步需要设置阻塞队列的长度，同步则不需要
bool Log::init(const char* file_name, int log_buf_size, int split_lines, int max_queue_size){
    // 同步情况下,max_queue_size 设置为0
    if(max_queue_size >= 1){  
        m_is_asyn = true;
        m_log_queue = new block_queue<string>(max_queue_size);  // 等待缓冲的数据
        pthread_t tid;
        //flush_log_thread为回调函数,这里表示创建线程异步写日志
        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }
  
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];        // 创建日志缓冲区
    memset(m_buf, '\0', m_log_buf_size);
    m_split_lines = split_lines;             // 单个文件最大长度

    time_t t = time(NULL);
    struct tm *sys_tm = localtime(&t);       // tm结构体，格式化的时间记录
    struct tm my_tm = *sys_tm;

    const char* p = strchr(file_name, '/');  // 定位给定的文件名
    char log_full_name[256] = {0};

    // 确定日志的名字
    if(p == NULL){
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }else{
        strcpy(log_name, p + 1);
        strncpy(dir_name, file_name, p - file_name + 1);
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    // 初始化系统日期，以及打开或创建缓冲日志
    m_today = my_tm.tm_mday;
    m_fp = fopen(log_full_name, "a");
    if(m_fp == NULL){
        return false;
    }
    return true;
}

void Log::write_log(int level, const char* format, ...){
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);      // 当前时间
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);   
    struct tm my_tm = *sys_tm;    // 格式化
    char s[16] = {0};
    switch(level){
        case 0:
            strcpy(s, "[debug]:");
            break;
        case 1:
            strcpy(s, "[info]:");
            break;
        case 2:
            strcpy(s, "[warn]:");
            break;
        case 3:
            strcpy(s, "[error]:");
            break;
        default:
            strcpy(s, "[info]:");
            break;
    }

    m_mutex.lock();
    m_count++;

    if(m_today != my_tm.tm_mday || m_count % m_split_lines == 0){   // 定天创建新文件缓存日志
        char new_log[256] = {0};                                   // 新文件
        fflush(m_fp);                                             // 刷新缓存区
        fclose(m_fp);                                            // 关闭旧缓冲区
        char tail[16] = {0};

        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);

        if(m_today != my_tm.tm_mday){         // 新的一天到了，要换一个日志文件存储了
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        }else{
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines); // 当天第k个日志文件，因为日志文件大小有限
        }
        m_fp = fopen(new_log, "a");
    }

    m_mutex.unlock();

    va_list valst;
    va_start(valst, format);

    string log_str;
    m_mutex.lock();

    // 写入的具体时间内容格式
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s", 
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    
    int m = vsnprintf(m_buf + n, m_log_buf_size - 1 - n, format, valst);  // 日志记录
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    if(m_is_asyn && !m_log_queue->full()){
        m_log_queue->push(log_str);
    }else{
        m_mutex.lock();   // 同步就是来一个写一个
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }
    va_end(valst);
}

void Log::flush(){
    m_mutex.lock();
    //强制刷新写入流缓冲区
    fflush(m_fp);
    m_mutex.unlock();
}