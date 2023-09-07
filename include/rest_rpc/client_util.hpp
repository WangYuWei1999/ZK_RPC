#pragma once
#include<tuple>

#if __cplusplus > 201402L
#include<string_view>
using string_view = std::string_view;  //c++ 17 字符串视图 查看操作性能高 不拥有字符串所有权
#else
#include"string_view.hpp"
using namespace nonstd;
#endif

#include"codec.h"

namespace rest_rpc{
inline bool has_error(string_view result){
    if(result.empty()) return true;
    
    rpc_service::msgpack_codec codec;   //缓冲区相关
    auto tp= codec.unpack<std::tuple<int>>(result.data(),result.size());

    return std::get<0>(tp)!=0;
}

template<typename T>
inline T get_result(string_view result){
    rpc_service::msgpack_codec codec;
    auto tp = codec.unpack<std::tuple<int,T>>(result.data(),result.size());
    return std::get<1>(tp);
}

inline std::string get_error_msg(string_view result){
    rpc_service::msgpack_codec codec;
    auto tp = codec.unpack<std::tuple<int,std::string>>(result.data(),result.size());
    return std::get<1>(tp);
}

template<typename T>
inline T as(string_view result){
    if(has_error(result)){
        throw std::logic_error(get_error_msg(result));
    }
    return get_result<T>(result);
}

}

//std::get(std::tuple) 
//获取元组数据 std::get<1>(tuple) 获取第一个数据 
//std::get<int>(t) << ", " << std::get<const char*>(t) 获取int类型和const char* 类型的数据