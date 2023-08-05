#ifndef LOG_H
#define LOG_H

#include<cstdio>
#include<iostream>
#include<string>
#include<cstdarg>
#include<pthread.h>
#include"block_queue.h"
using std::string;

class Log{
public:
    static Log* get_instance(){
        static Log instance;
        return &instance;
    }

    static void* flush_log_thread(void* args){
        Log::get_instance()->async_write_log();
        return args;  // 多余的
    }

    // 可选择的参数有日志文件、日志缓冲区大小、最大函数以及最长日志条队列
    bool init(const char* file_name, int log_buf_size = 8192, int split_lines = 5e6, int max_queue_size = 0);

    void write_log(int level, const char* format, ...);

    void flush(void);
private:
    Log();  // 不能实例化
    virtual ~Log();
    void *async_write_log(){
        string string_log;
        // 从阻塞队列中取出一个日志string, 写入到文件
        while(m_log_queue->pop(string_log)){
            m_mutex.lock();
            fputs(string_log.c_str(), m_fp);
            m_mutex.unlock();
    }
}
private:
    char dir_name[128];        //路径名
    char log_name[128];       // log文件名
    int m_split_lines;       // 日志最大行数
    int m_log_buf_size;     // 日志缓冲区大小
    long long m_count;     // 日志行数记录
    int m_today;          // 因为按天分类，记录当前时间是哪一天
    FILE* m_fp;          // 打开log的文件指针
    char* m_buf;
    block_queue<string>* m_log_queue;    //阻塞队列
    bool m_is_asyn;                      // 是否同步的标志
    locker m_mutex;
};


#define LOG_DEBUG(format, ...) Log::get_instance()->write_log(0, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...) Log::get_instance()->write_log(1, format, ##__VA_ARGS__)
#define LOG_WARN(format, ...) Log::get_instance()->write_log(2, foramt, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) Log::get_instance()->write_log(3, format,  ##__VA_ARGS__)

#endif