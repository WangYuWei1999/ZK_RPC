#pragma once
#include <unordered_map>
#include <string>

#define BUFFER_SIZE 128

namespace rest_rpc{
namespace rpc_service{

//单例模式查询本机ip zk客户端ip

/*
* 框架读取配置文件类
* rpcserver:ip rpcserver:port
* zookeeper:ip zookeeper:port
*/
class RpcConfigure
{
public:

    static RpcConfigure& get_configure(){
        static RpcConfigure configure;
        return configure;
    }

        //查询配置项信息
   static std::string find(std::string key)
{
    auto it = configure_map_.find(key);
    if (it == configure_map_.end())
    {
        return "";
    }
    return it->second;
}

private:
    //加载配置文件 注意：不能为private！！！
    static void load(const char *config_file)
{
    FILE *pf = fopen(config_file, "r");
    if (pf == nullptr)
    {
        //RPC_LOG_FATAL("%s is not exist!", config_file);
    }

    // 1.注释 2.正确的配置项 3.去掉开头多余的空格
    while (!feof(pf))
    {
        char buf[BUFFER_SIZE] = {0};
        fgets(buf, BUFFER_SIZE, pf);

        std::string str_buf(buf);

        trim(str_buf);

        //判断# 注释 或者空行
        if (str_buf[0] == '#' || str_buf[0] == '\n' || str_buf.empty())
        {
            continue;
        }

        // 解析配置项
        int index = str_buf.find('=');
        if (index == -1)
        {
           // RPC_LOG_ERROR("configure file illegal!");
            continue;
        }

        std::string key = str_buf.substr(0, index);
        trim(key);

        std::string value = str_buf.substr(index + 1, str_buf.size() - index);
        //去除最后一个换行符'\n'
        value[value.size() - 1] = ' ';
        trim(value);

        configure_map_.insert({key, value});
    }
}

    //去掉字符串前后的空格
  static void trim(std::string &str_buf)
{
    //检查空格并去掉
    //找到第一个不为空格的字符
    int index = str_buf.find_first_not_of(' ');
    if (index != -1)
    {
        //说明前面有字符，截取字符
        str_buf = str_buf.substr(index, str_buf.size() - index);
    }
    //去掉字符串后面的空格，找到最后一个不为空格的字符
    index = str_buf.find_last_not_of(' ');
    if (index != -1)
    {
        //说明后面有空格，截取字符
        str_buf = str_buf.substr(0, index + 1);
    }
}

private:
    RpcConfigure(){
        load("rpc.conf");  //加载本地文件rpc.conf
    }

    ~RpcConfigure(){}

    RpcConfigure& operator=(const RpcConfigure&) = delete;
    RpcConfigure(const RpcConfigure&) = delete;

    static std::unordered_map<std::string, std::string> configure_map_;
};

std::unordered_map<std::string, std::string> RpcConfigure::configure_map_;

}
}