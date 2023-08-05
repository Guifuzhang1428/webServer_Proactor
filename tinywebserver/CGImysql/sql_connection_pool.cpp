#include<mysql/mysql.h>
#include<cstdlib>
#include<pthread.h>
#include "sql_connection_pool.h"

using std::string;
connection_pool::connection_pool():CurrConn(0), FreeConn(0){}

// 功能是什么？创建一个连接池？
connection_pool* connection_pool::GetInstance(){
    static connection_pool connPool;
    return &connPool;
}

// 初始化mysql连接
void connection_pool::init(string url, string User, string PassWord, string DataBaseName, int Port, unsigned int MaxConn){
    this->url = url;
    this->Port = Port;
    this->User = User;
    this->PassWord = PassWord;
    this->DataBaseName = DataBaseName;

    lock.lock();
    for (int i = 0; i < MaxConn; i++){
        MYSQL* con = NULL;
        con = mysql_init(con);   // mysql 初始化API

        if(con == NULL){
            std::cerr << "Error:" << mysql_errno(con);
            exit(1);
        }

        con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DataBaseName.c_str(), Port, NULL, 0); // 数据库的连接

        if(con == NULL){
            std::cerr << " Error:" << mysql_errno(con) << std::endl;
            exit(1);
        }
        connList.push_back(con);
        ++FreeConn;
    }

    reserve = sem(FreeConn);
    this->MaxConn = FreeConn;
    lock.unlock();
}

// 当有请求时，从数据库连接池中返回一个可用的连接，更新使用和空闲的连接数
MYSQL* connection_pool::GetConnection(){
    MYSQL* con = NULL;
    if(connList.empty()){
        return con;
    }
    reserve.wait();
    lock.lock();
    con = connList.front();
    connList.pop_front();
    --FreeConn;
    ++CurrConn;
    lock.unlock();
    return con;
}

// 释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL* conn){
    if(conn == NULL){return false;}
    lock.lock();
    connList.push_back(conn);
    ++FreeConn;
    --CurrConn;
    lock.unlock();
    reserve.post();
    return true;
}

// 销毁所有的连接
void connection_pool::DestroyPoll(){
    lock.lock();
    if(connList.size() > 0){
        std::list<MYSQL*>::iterator it;
        for(it = connList.begin(); it!=connList.end(); ++it){
            MYSQL* con = *it;
            mysql_close(con);
        }
        CurrConn = 0;
        FreeConn = 0;
        connList.clear();
    }
    lock.unlock();
}

// 获取当前空闲的连接数
int connection_pool::GetFreeConn(){
    return this->FreeConn;
}

connection_pool::~connection_pool(){
    DestroyPoll();
}

// 用于记录取出的sql线程信息
connectionRAII::connectionRAII(MYSQL** conn, connection_pool* connPool){
    *conn = connPool->GetConnection();  // 获取一个空闲的连接

    connRAII = *conn;
    poolRAII = connPool;
}

connectionRAII::~connectionRAII(){
    poolRAII->ReleaseConnection(connRAII);
}