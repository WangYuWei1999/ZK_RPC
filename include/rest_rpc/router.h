#ifndef REST_RPC_ROUTER_H_
#define REST_RPC_ROUTER_H_
#include"codec.h"
#include"md5.hpp"
#include"meta_util.hpp"
#include"use_asio.hpp"
#include<functional>
#include<string>
#include<unordered_map>

//注册服务端函数到哈希表中

namespace rest_rpc{
//enum class是限定作用域枚举型别，他们仅在枚举型别内可见，且只能通过强制转换转换为其他型别。
enum class ExecMode{sync,async};   //工作模式 同步、异步
const constexpr ExecMode Async = ExecMode::async;   //enum class 枚举使用前要加限定符号

namespace rpc_service{
class connection;
class router:asio::noncopyable{
public:
    //注册非成员函数
    template<ExecMode model, typename Function>  
    void register_handler(const std::string& name, Function f){
        //从name哈希出key
        uint32_t key = MD5::MD5Hash32(name.data());      //string.data()与.c_str()相同
        key2func_name_.emplace(key, name);                //将key和name加入到哈希表中
        return register_nonmember_func<model>(key, std::move(f));   //继续注册到
    }

    //注册成员函数
    template<ExecMode model, typename Function, typename Self>   
    void register_handler(const std::string& name, const Function& f, Self& self){
        uint32_t key = MD5::MD5Hash32(name.data());
        key2func_name_.emplace(key,name);
        return register_member_func<model>(key, f, self);
    }

    //移除注册函数
    void remove_handler(const std::string& name){
        uint32_t key = MD5::MD5Hash32(name.data());
        this->map_invokers_.erase(key);
        key2func_name_.erase(key);
    }

    //返回名字
    std::string get_name_by_key(uint32_t key){
        auto it = key2func_name_.find(key);
        if(it!=key2func_name_.end()){
            return it->second;
        }
        return std::to_string(key);
    }

    template<typename T>
    void route(uint32_t key, const char* data, std::size_t size, std::weak_ptr<T> conn){
        //lock()如果当前 weak_ptr 已经过期，则该函数会返回一个空的 shared_ptr 指针；反之，该函数返回一个和当前 weak_ptr 指向相同的 shared_ptr 指针。
        auto conn_sp = conn.lock(); //返回shared_ptr或者
        if(!conn_sp) return;

        auto req_id = conn_sp->request_id(); //从connection连接获取请求id
        std::string result;
        try{
            msgpack_codec codec;
            auto it = map_invokers_.find(key);  //凭借key找到对应的调用方法
            if(it == map_invokers_.end()){
                result = codec.pack_args_str(result_code::FAIL,"unknown function:"+get_name_by_key(key));
                conn_sp->response(req_id,std::move(result));
                return;
            }

            ExecMode model;
            //调用对应方法处理请求
            it->second(conn,data,size,result,model); //（apply(),apply_member())
            if(model == ExecMode::sync){
                if(result.size() >= MAX_BUF_LEN){
                    result = codec.pack_args_str(result_code::FAIL, "the response result is out of range: more than 10M "+ get_name_by_key(key));
                }
                conn_sp->response(req_id, std::move(result));   //将处理结果返回连接connection中
            }
        }
        catch(const std::exception& ex){
            msgpack_codec codec;        //exception::what()用于获取字符串标识异常
            result = codec.pack_args_str(result_code::FAIL, ex.what());
            conn_sp->response(req_id,std::move(result));
        }
    }

    router() = default; //声明一个默认构造函数

private:
    router(const router&) =delete;
    router(router&& ) = delete;

    template<typename F, size_t... I, typename... Args>
    //头文件:<type_traits> 用于在编译的时候推导出一个可调用对象(函数,std::funciton或者重载了operator()操作的对象等)的返回值类型
    static typename std::result_of<F(std::weak_ptr<connection>,Args...)>::type call_helper(const F &f,const std::index_sequence<I...>&,
        std::tuple<Args...>tup,std::weak_ptr<connection> ptr){
            return f(ptr,std::move(std::get<I>(tup))...);
        }

    //非成员函数 F(...)返回值为void 走此版本 返回值void
    template<typename F, typename... Args>
    static typename std::enable_if<std::is_void<typename std::result_of<F(std::weak_ptr<connection>, Args...)>::type>::value>::type call(const F& f,std::weak_ptr<connection> ptr,std::string& result,std::tuple<Args...>tp){
        //std::make_index_sequnence<N>{} 构造函数在编译时期生成0- N-1的整数列
        call_helper(f, std::make_index_sequence<sizeof...(Args)>{}, std::move(tp), ptr);
        result = msgpack_codec::pack_args_str(result_code::OK);
    }

    //非成员函数 F(...)返回值不为void 走此版本 返回值void
    template<typename F, typename... Args>
    static typename std::enable_if<!std::is_void<typename std::result_of<F(std::weak_ptr<connection>, Args...)>::type>::value>::type call(const F&f, std::weak_ptr<connection> ptr, std::string& result,std::tuple<Args...>tp){
        auto r = call_helper(f, std::make_index_sequence<sizeof...(Args)>{}, std::move(tp), ptr);
        result = msgpack_codec::pack_args_str(result_code::OK, r);
    }

      template <typename F, typename Self, size_t... Indexes, typename... Args>
  static
      typename std::result_of<F(Self, std::weak_ptr<connection>, Args...)>::type
      call_member_helper(const F &f, Self *self,
                         const std::index_sequence<Indexes...> &,
                         std::tuple<Args...> tup,
                         std::weak_ptr<connection> ptr =
                             std::shared_ptr<connection>{nullptr}) {
    return (*self.*f)(ptr, std::move(std::get<Indexes>(tup))...);
  }

    //成员函数 F(...)返回值为 void走此版本 返回值void
  template <typename F, typename Self, typename... Args>
  static typename std::enable_if<std::is_void<typename std::result_of<
      F(Self, std::weak_ptr<connection>, Args...)>::type>::value>::type
    call_member(const F &f, Self *self, std::weak_ptr<connection> ptr,
              std::string &result, std::tuple<Args...> tp) {
    //func没有返回值；
    call_member_helper(f, self,                        
                       typename std::make_index_sequence<sizeof...(Args)>{},
                       std::move(tp), ptr);
    result = msgpack_codec::pack_args_str(result_code::OK);
  }

    //成员函数 F(...)返回值 不为void走此版本 返回值void
  template <typename F, typename Self, typename... Args>
  static typename std::enable_if<!std::is_void<typename std::result_of<
      F(Self, std::weak_ptr<connection>, Args...)>::type>::value>::type
  call_member(const F &f, Self *self, std::weak_ptr<connection> ptr,
              std::string &result, std::tuple<Args...> tp) {
    //func有返回值
    auto r = call_member_helper(
        f, self, typename std::make_index_sequence<sizeof...(Args)>{},
        std::move(tp), ptr);
    result = msgpack_codec::pack_args_str(result_code::OK, r);  //响应结果序列化
  }

    //调用程序 通过这里调用经过注册的本地方法
    template<typename Function, ExecMode mode = ExecMode::sync> 
    struct invoker{
        template<ExecMode model>  //模板函数
        static inline void apply(const Function& func, std::weak_ptr<connection> conn, const char* data,
            size_t size, std::string& result, ExecMode& exe_model){
                using args_tuple = typename function_traits<Function>::bare_tuple_type;
                exe_model = ExecMode::sync;
                msgpack_codec codec;
                try{
                    auto tp = codec.unpack<args_tuple>(data,size);  //参数反序列化
                    call(func,conn,result,std::move(tp));
                    exe_model = model;
                }
                catch(std::invalid_argument& e){
                    result = codec.pack_args_str(result_code::FAIL,e.what());
                }
                catch(const std::exception& e){
                    result = codec.pack_args_str(result_code::FAIL,e.what());
                }
            }
        
        template <ExecMode model, typename Self>
    static inline void apply_member(const Function &func, Self *self,
                                    std::weak_ptr<connection> conn,
                                    const char *data, size_t size,
                                    std::string &result, ExecMode &exe_model) {
      using args_tuple = typename function_traits<Function>::bare_tuple_type;
      exe_model = ExecMode::sync;
      msgpack_codec codec;
      try {
        auto tp = codec.unpack<args_tuple>(data, size);       //参数反序列化
        call_member(func, self, conn, result, std::move(tp));
        exe_model = model;
      } catch (std::invalid_argument &e) {
        result = codec.pack_args_str(result_code::FAIL, e.what());
      } catch (const std::exception &e) {
        result = codec.pack_args_str(result_code::FAIL, e.what());
      }
    }
    };

    //注册非成员函数
    template<ExecMode model, typename Function>
    void register_nonmember_func(uint32_t key, Function f){
        this->map_invokers_[key] = {std::bind(               //将函数指针作为参数绑定到apply函数中
            &invoker<Function>::template    //绑定了模板函数
            apply<model>, std::move(f),
            std::placeholders::_1, std::placeholders::_2, std::placeholders::_3,
            std::placeholders::_4, std::placeholders::_5)};
    }

    //注册成员函数
    template<ExecMode model, typename Function, typename Self>
    void register_member_func(uint32_t key, const Function& f, Self* self){
        this->map_invokers_[key] = {std::bind(                        //将函数指针及其所属对象作为参数绑定到apply_member函数中作为可调用对象
            &invoker<Function>::template apply_member<model, Self>, f, self,
            std::placeholders::_1, std::placeholders::_2, std::placeholders::_3,
            std::placeholders::_4, std::placeholders::_5)};
    }

private:
    //存放key 调用本地方法的函数
    std::unordered_map<uint32_t,std::function<void(std::weak_ptr<connection>, const char*, size_t, std::string&, ExecMode& model)>> map_invokers_;
    //存放从函数名哈希出的key 和 函数名
    std::unordered_map<uint32_t,std::string> key2func_name_;
};
}
}

#endif