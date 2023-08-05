#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<cstdio>
#include<unistd.h>
#include<cerrno>
#include<fcntl.h>
#include<cstdlib>
#include<sys/epoll.h>

#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./timer/lst_timer.h"
#include "./http/http_conn.h"
#include "./log/log.h"
#include "./CGImysql/sql_connection_pool.h"

#define MAX_FD 65535                   //最大文件描述符
#define MAX_EVENT_NUMBER 10000        // 最大事件数
#define TIMESLOT 5

#define SYNLOG    // 同步写日志
// #define ASYNLOG  // 异步写日志

// #define listenfdET  // 边缘触发非阻塞
#define listenfdLT     // 水平触发非阻塞

// 引入外部函数声明，用于改变链接属性
// extern int addfd(int epollfd, int fd, bool one_shot);
extern void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);
extern int removefd(int epollfd, int fd);
extern int setnonblocking(int fd);

// 设置定时器相关参数
static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;

// 快速设置IP
void SetIP(sockaddr_in& addr, const char* ip, int port){
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr.s_addr);
}


//信号处理函数
void sig_handler(int sig){
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

// 设置信号函数
void addsig(int sig, void(*handler)(int), bool restart = true){
    sigset_t _sig;
    sigemptyset(&_sig);
    sigfillset(&_sig);       // 处理信号的时候阻塞

    struct sigaction act;
    act.sa_flags = 0;
    act.sa_handler = handler;
    if(restart){
        act.sa_flags |= SA_RESTART;  // 恢复自动中断的系统调用，而不是返回错误号
    }
    act.sa_mask = _sig; 
    assert(sigaction(sig, &act, NULL) != -1);
}


// 定时处理任务，重新定时以不断触发SIGALRM
void timer_handler(){
    timer_lst.tick();
    alarm(TIMESLOT);
}

// 定时器回调函数，删除非活动连接在socket上的注册时间，并关闭
void cb_func(client_data *user_data){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, NULL);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
    LOG_INFO("close fd %d", user_data->sockfd);
    Log::get_instance()->flush();
}

// 向连接出错的连接发送错误信息
void show_error(int connfd, const char* info){
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[]){
#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8); // 异步日志模型
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 20480, 800000, 0); // 同步日志模型
#endif

    if(argc <= 1){
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi(argv[1]);   // 端口

    addsig(SIGPIPE, SIG_IGN);  // 防止客户端断开后，服务器再往里面写数据而导致的管道破裂

    // 创建数据库连接池, 获取一个静态对象
    connection_pool* connPool = connection_pool::GetInstance();
    connPool->init("localhost", "root", "password", "yourdb", 3306, 8); // 服务器连接数据库， 设置自己的用户和密码

    // 创建线程池
    threadpool<http_conn>* pool = NULL;
    try{ //int thread_number = 8, int max_request = 10000
        pool = new threadpool<http_conn>(connPool);  
    }catch(...){
        return 1;
    }

    http_conn* users = new http_conn[MAX_FD];   // 最大连接数
    assert(users);

    // 初始化数据库读取表,这里initmysql_result只调用一次，将数据库链表中的数据导入到map中
    users->initmysql_result(connPool);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    //struct linger tmp={1,0};
    //SO_LINGER若有数据待发送，延迟关闭
    //setsockopt(listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));

    int ret = 0;
    struct sockaddr_in address;
    SetIP(address, "xxx.xxx.xxx.xxx", port); // 提供IP设置API, 设置自己的IP地址

    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));    // 端口复用
    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));     // 绑定
    assert(ret >= 0);

    ret = listen(listenfd, 5);
    assert(ret >= 0);

    // 创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);

#ifdef listenfdLT
    // addfd(epollfd, listenfd, false); // 有bug
    addfd(epollfd, listenfd, false, 0);
#endif

#ifdef listenfdET
    addfd(epollfd, listenfd, false, 1);
#endif

    http_conn::m_epollfd = epollfd;

    // 创建管道，用于接收定时信号
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);            // 写数据非阻塞
    addfd(epollfd, pipefd[0], false, 0); // 管道也是非oneshot的
    
    // 捕获信号
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);
    bool stop_server = false;

    client_data* users_timer = new client_data[MAX_FD];

    bool timeout = false;
    alarm(TIMESLOT);

    while(!stop_server){
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if(number == -1 && errno != EINTR){  // EINTR信号的接受
            LOG_ERROR("%s", "epoll failure");
            break;
        }else if(number == -1 && (errno == EINTR)){
            continue;
        }else if(number == 0){
            continue;
        }
        

        for(int i = 0; i < number; i++){
            int sockfd = events[i].data.fd;
            
            // 处理新到的客户连接
            if(sockfd == listenfd){
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
#ifdef listenfdLT
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlength);
                if(connfd < 0){
                    LOG_ERROR("%s:errno is:%d", "accept error", errno);
                    continue;
                }
                if(http_conn::m_user_count >= MAX_FD){
                    show_error(connfd, "Internal server busy");
                    LOG_ERROR("%s", "Internal server busy");
                    continue;
                }

                users[connfd].init(connfd, client_address);

                // 初始化client_data数据
                // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                util_timer *timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer);
#endif

#ifdef listenfdET
                while(1){
                    int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlength);
                    if(connfd < 0){
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);
                        break;
                    }
                    if(http_conn::m_user_count >= MAX_FD){
                        show_error(connfd, "Internal server busy");
                        LOG_ERROR("%s", "Internal server busy");
                        break;
                    }
                    users[connfd].init(connfd, client_address);
                    // 初始化client_data数据
                    // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    users_timer[connfd].timer = new util_timer;
                    time_t curr_tm = time(NULL);
                    users_timer[connfd].timer->expire = curr_tm + 3 * TIMESLOT;
                    users_timer[connfd].timer->cb_func = cb_func;    
                    users_timer[connfd].timer->user_data = users_timer + connfd;
                    timer_lst.add_timer(users_timer[connfd].timer);
                }
                continue;
#endif
            }
            // 处理连接中的错误
            else if (events[i].events & (EPOLLRDHUP | EPOLLERR | EPOLLHUP)){
                // 服务器关闭连接,移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);
                if(timer){
                    timer_lst.del_timer(timer);
                }
            }

            // 处理管道中的信号
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)){
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if(ret == -1){
                    continue;
                }else if(ret == 0){
                    continue;
                }else{
                    for(int i = 0; i < ret; ++i){
                        switch(signals[i]){
                            case SIGALRM:
                                timeout = true;
                                break;
                            case SIGTERM:
                                stop_server = true;
                                break;
                        }   
                    }
                }
            }else if(events[i].events & EPOLLIN){
                if(users[sockfd].read_once()){
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    // 若检测到读事件，将改时间放入到请求队列
                    pool->append(users + sockfd);

                    // 若有数据传输，则将定时器往后延迟3个单位
                    if(users_timer[sockfd].timer){
                        users_timer[sockfd].timer->expire = time(NULL) + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(users_timer[sockfd].timer);
                    }
                }else{
                    users_timer[sockfd].timer->cb_func(&users_timer[sockfd]);
                    if(users_timer[sockfd].timer){
                        timer_lst.del_timer(users_timer[sockfd].timer);
                    }
                }
            }else if(events[i].events & EPOLLOUT){
                util_timer* timer = users_timer[sockfd].timer;
                if(users[sockfd].write()){
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    // 若有数据传输，则将定时器完后延迟3个单位
                    if(time){
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }   
                }else{
                    timer->cb_func(&users_timer[sockfd]);
                    if(timer){
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }

        // 将过期的连接移除
        if(timeout){
            timer_handler();
            timeout = false;
        }
    }

    close(epollfd);
    close(listenfd);
    close(pipefd[0]);
    close(pipefd[1]);
    delete [] users;
    delete [] users_timer;
    delete pool;
    return 0;       
}