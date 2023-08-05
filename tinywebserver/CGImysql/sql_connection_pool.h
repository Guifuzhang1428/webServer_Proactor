#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include<cstdio>
#include<list>
#include<mysql/mysql.h>
#include<cerrno>
#include<cstring>
#include<iostream>
#include<string>
#include"../lock/locker.h"

using std::string;

class connection_pool{
public:
    MYSQL* GetConnection();               // 获取数据库连接
    bool ReleaseConnection(MYSQL* conn);  // 释放连接
    int GetFreeConn();                    // 获取连接
    void DestroyPoll();                   // 销毁所有的连接

    // 单例模式
    static connection_pool* GetInstance();
    void init(string url, string User, string PassWord, string DataBaseName, int Port, unsigned int MaxConn);

    connection_pool();
    ~connection_pool();

private:
    unsigned int MaxConn;   //Mysql最大连接数
    unsigned int CurrConn;  // 当前连接数
    unsigned int FreeConn;  // 当前空闲的连接数

private:
    locker lock;
    std::list<MYSQL*> connList;   // 连接池
    sem reserve;             // 需要处理的任务数量

private:
    string url;             // 主机地址
    string Port;           // 数据库端口号
    string User;          // 登录数据库用户名
    string PassWord;     // 登录数据库密码
    string DataBaseName;  // 使用数据库的名字
};

class connectionRAII{

public:
    connectionRAII(MYSQL** conn, connection_pool* connPool);
    ~connectionRAII();

private:
    MYSQL* connRAII;
    connection_pool* poolRAII;
};
#endif
