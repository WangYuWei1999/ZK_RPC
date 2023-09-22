#ifndef REST_RPC_ZOOKEEPERCLIENT_H_
#define REST_RPC_ZOOKEEPERCLIENT_H_

#pragma once
#include<iostream>
#include <semaphore.h>
#include <zookeeper/zookeeper.h>
#include <string>
#include<string.h>
#include"configure.hpp"

namespace rest_rpc{
//namespace rpc_service{

void global_watcher(zhandle_t *handler, int type, int state, const char *path, void *wathcer_context)
{
    if (type == ZOO_SESSION_EVENT) //回调的消息类型
    {
        //连接成功就会发送一个成功信号
        if (state == ZOO_CONNECTED_STATE) //zkclient 和 zkserver 连接成功
        {
            //获取信号量
            sem_t *sem = (sem_t *)zoo_get_context(handler);
            sem_post(sem);
        }
    }
}


class ZookeeperClient
{
public:
    ZookeeperClient():zhandle_(nullptr){start();}

    ~ZookeeperClient(){
    if (zhandle_ != nullptr){
            zookeeper_close(zhandle_); //关闭句柄，释放资源
        }
    }

    //启动连接 zkserver
    void start(){
        //zk服务端的ip和端口
        // std::string host = RpcConfigure::get_configure().find("zookeeper_ip");
        // std::string port = RpcConfigure::get_configure().find("zookeeper_port");
        std::string host = "127.0.0.1";
        std::string port = "2181";
        std::string con_str = host + ":" + port;
        std::cout<<con_str<<std::endl;

        zhandle_ = zookeeper_init(con_str.c_str(), global_watcher, 30000, nullptr, nullptr, 0);
        if (zhandle_ == nullptr)
        {
        // RPC_LOG_FATAL("zookeeper init error");
        std::cout<<"zookeeper init error"<<std::endl;
        }

        sem_t sem;
        sem_init(&sem, 0, 0);
        zoo_set_context(zhandle_, &sem); //设置信号量s
        sem_wait(&sem);
        //RPC_LOG_INFO("zookeeper init success");
    }
    
    //在zkserver 根据指定的path创建znode节点
    void create(const char *path, const char *data, int datalen, int state = 0){
        char path_buffer[128] = {0};
        int buffer_len = sizeof(path_buffer);
        int flag;

        //同步检查path是否存在
        flag = zoo_exists(zhandle_, path, 0, nullptr);
        if (ZNONODE == flag) //不存在
        {
            flag = zoo_create(zhandle_, path, data, datalen, &ZOO_OPEN_ACL_UNSAFE, state, path_buffer, buffer_len);
            if (flag == ZOK) //成功
            {
                //RPC_LOG_INFO("znode create  success...path: %s", path);
                std::cout<<"znode create  success...path"<<std::endl;
            }
            else
            {
                //RPC_LOG_FATAL("falg: %d, znode create error... path: %s", flag, path);
                std::cout<<"znode create error... path"<<std::endl;
            }
        }
    }

    void create_method(const std::string& serpath, const std::string& methodname, std::string& ip, int port, int state = 0){
        std::string method_path = serpath + "/" + methodname;
        char method_ip_port[128] = {0};
        sprintf(method_ip_port, "%s:%d", ip.c_str(), port);  //将ip和端口写入
        create(method_path.c_str(), method_ip_port, strlen(method_ip_port), state);
    }

    //根据参数指定的znode节点路径，获取znode节点的值
    std::pair<std::string, unsigned short> get_data(const char *path){
         //buffer存储返回结果
        char buffer[64] = {0};
        int buffer_len = sizeof(buffer);
        int flag = zoo_get(zhandle_, path, 0, buffer, &buffer_len, nullptr);
        if (flag != ZOK)
        {
            //RPC_LOG_ERROR("can't get znode... path: %s", path);
            std::cout<<"can't get znode... path"<<std::endl;
            return {"",0};
        }
        else
        {
            std::string name_ip_port = buffer;
            int host_index = name_ip_port.find(":");
            if(host_index == -1){
                std::cout<<"address is invalid!"<<std::endl;
            }
            std::string host = name_ip_port.substr(0, host_index);
            unsigned short port = atoi(name_ip_port.substr(host_index+1, name_ip_port.size()-host_index).c_str());
            return std::make_pair(host, port);
           // return {host, port};
        }
    }

private:
    //zk的客户端句柄
    zhandle_t *zhandle_;
};

//} //namespace rpc_service
} //namwspace rest_rpc
#endif