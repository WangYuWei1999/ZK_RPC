#pragma once
#include<cstdint>
#include<memory>

//const_vars 常量——变量

 namespace rest_rpc{
    enum class result_code:std::int16_t{
        OK=0, FAIL=1,
    };

    enum class error_code{
        OK,UNKNOWN,FALL,TIMEOUT,CANCEL,BADCONNECTION,
    };

    //报文请求类型
    enum class request_type:uint8_t{
        req_res,sub_pub
    };

    //响应消息结构体 消息类型，消息id，消息内容
    struct message_type{
        std::uint64_t req_id;       //请求id
        request_type req_type;      //请求类型
        std::shared_ptr<std::string> content;   //响应内容
    };

    static const uint8_t MAGIC_NUM = 39;

    //定义rpc头部机构体，头部一些组成元素
    struct rpc_header
    {   
        uint8_t magic;
        request_type req_type;  //请求类型
        uint32_t body_len;      //请求体长度
        uint64_t req_id;        //请求id
        uint32_t func_id;       //请求函数哈希id
    };

    static const size_t MAX_BUF_LEN = 1048576 * 10;
    static const size_t HEAD_LEN = sizeof(rpc_header);  //rpc头部长度固定
    static const size_t INIT_BUF_SIZE = 2*1024;
    
 }