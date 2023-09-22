#pragma once    
#include"client_util.hpp"
#include"const_vars.h"
#include"md5.hpp"
#include"meta_util.hpp"
#include"use_asio.hpp"
#include"zookeeperclient.hpp"
#include<deque>
#include<functional>
#include<future>
#include<iostream>
#include<string>
#include<thread>
#include<utility>

#include"zookeeperclient.hpp"

using namespace rest_rpc::rpc_service;

namespace rest_rpc{

class req_result{
public:
    req_result() = default;
    req_result(string_view data):data_(data.data() , data.length()){}
    bool success() const {return !has_error(data_);} //使用工具函数检查

    //检查接受的数据是否有误以及 反序列化接受数据
    template<typename T>
    T as(){
        if(has_error(data_)){
            std::string err_msg = data_.empty()?data_ : get_error_msg(data_);
            throw std::logic_error(err_msg);
        }
        return get_result<T>(data_);  //反序列化
    }

    void as(){
        if(has_error(data_)){
            std::string err_msg = data_.empty()?data_:get_error_msg(data_);
            throw std::logic_error(err_msg);
        }
    }
    
private:
    std::string data_;
};

//获取线程返回异步任务结果，使用全局变量 或者使用std::future<T>；唯一期望std::future，共享期望std::shared_future
template<typename T>
struct future_result{
    uint64_t id;
    std::future<T> future; //std::future获取异步操作结果
    template<class Rep, class Per>
    //future_status枚举常量enum class future_status{ready,timeout,deferred};
    std::future_status wait_for(const std::chrono::duration<Rep,Per>& rel_time){
        return future.wait_for(rel_time); //等待异步操作结果
    }
    //get()得到异步操作结果
    T get(){return future.get();}//等价于先调用wait()后调用get()
};

enum class CallModel {future, callback};  //future接口和callback接口
const constexpr auto FUTURE = CallModel::future;

const constexpr size_t  DEFAULT_TIMEOUT = 5000; //millseconds

//rpc客户端
class rpc_client:private asio::noncopyable{
public:
    rpc_client():socket_(ios_), work_(ios_), deadline_(ios_), body_(INIT_BUF_SIZE){
        thd_ = std::make_shared<std::thread> ([this]{ios_.run();});
    }

    rpc_client( std::function<void(long, const std::string&)>on_result_received_callback)
        :socket_(ios_), work_(ios_), deadline_(ios_), body_(INIT_BUF_SIZE),
            on_result_received_callback_(std::move(on_result_received_callback)){
                thd_ = std::make_shared<std::thread>([this]{ios_.run();});
            }

    rpc_client(const std::string& host, unsigned short port):rpc_client(nullptr, host, port){}

    //新增构造函数，构造对象时值只需传入服务端的名称，由zk客户端自行查询到服务端ip+端口
    rpc_client(const std::string& servername, std::function<void(long, const std::string&)> on_result_received_callback = nullptr):socket_(ios_), 
    work_(ios_), deadline_(ios_), body_(INIT_BUF_SIZE), on_result_received_callback_(std::move(on_result_received_callback)){
        std::string serpath = "/"+servername;   //服务端在zk上的注册路径为 /servername
        auto ser_ip_port = zk_client_.get_data(serpath.c_str());
        if(ser_ip_port.first == ""){
            std::cout<<"查询失败！"<<std::endl;            
        }        

        //从zk查询到服务端ip和端口
        host_ = ser_ip_port.first;
        port_ = ser_ip_port.second;

        thd_ = std::make_shared<std::thread>([this]{ios_.run();});
    }

    rpc_client(std::function<void(long, const std::string&)> on_result_received_callback, std::string host, unsigned short port)
        :socket_(ios_), work_(ios_), deadline_(ios_), host_(std::move(host)), port_(port), body_(INIT_BUF_SIZE),
            on_result_received_callback_(std::move(on_result_received_callback)){
                thd_= std::make_shared<std::thread>([this]{ios_.run();});
            }

    ~rpc_client(){
        close();
        stop();
    }

    void run(){
        if(thd_!=nullptr && thd_->joinable()) thd_->join();
    }

    void set_connect_timeout(size_t milliseconds){
        connect_timeout_ = milliseconds;
    }

    void set_reconnect_count(int reconnect_count){
        reconnect_cnt_ = reconnect_count;
    }

    bool connect(size_t timeout = 3, bool is_ssl = false){
        if(has_connected_) return true;

        assert(port_!=0);
        if(is_ssl) upgrade_to_ssl();
        async_connect();
        return wait_conn(timeout);
    }

    //参数只给服务端名称，通过zookeeper查询服务端ip端口
    bool connect(const std::string& servername){
        std::string ser_path = "/"+servername;                  //查询要带上根路径“/”
        auto ip_port = zk_client_.get_data(ser_path.c_str());
        bool r = connect(ip_port.first, ip_port.second);
        return r;
    }

    bool connect(const std::string& host, unsigned short port, bool is_ssl = false, size_t timeout = 3){
        if(port_==0){
            host_ = host;
            port_ = port;
        }
        return connect(timeout, is_ssl);
    }

    void async_connect(const std::string& host, unsigned short port){
        if(port_== 0){
            host_ = host;
            port_ = port;
        }
        async_connect();
    }

    //等待连接，这里用条件变量线程间通信
    bool wait_conn(size_t timeout){
        if(has_connected_) return true;
        
        has_wait_ = true;
        std::unique_lock<std::mutex> lock(conn_mtx_);
        //首先等待标志设置为true，上锁；条件变量阻塞当前线程 等待其他其他线程唤醒 timeout时间后没有被唤醒则自动解锁程序继续运行，等待标志设置为false
        bool result = conn_cond_.wait_for(lock, std::chrono::seconds(timeout),[this]{return has_connected_.load();});
        has_wait_ = false;
        return has_connected_;
    }

    void enable_auto_reconnect(bool enable = true){
        enable_reconnect_ = enable;
        reconnect_cnt_ = std::numeric_limits<int>::max(); //数值极值 头文件<limits> int最大值2147483647
    }

    void enable_auto_heartbeat(bool enable = true){
        if(enable){
            reset_deadline_timer(5);
        }else{
            deadline_.cancel();
        }
    }

    void update_addr(const std::string& host, unsigned short port){
        host_ = host;
        port_ = port;
    }

    void close(bool close_ssl = true){
        asio::error_code ec;
        if(close_ssl){
#ifdef CINATRA_ENABLE_SSL
            if(ssl_stream_){
                ssl_stream_->shutdown(ec);
                ssl_stream_ = nullptr;
            }
#endif
        }
        if(!has_connected_) return;
        has_connected_ = false;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
        clear_cache();
    }

    void set_error_callback(std::function<void(asio::error_code)> f){
        err_cb_ = std::move(f);
    }

    uint64_t reqest_id(){return temp_req_id_;}

    bool has_connected() const {return has_connected_;}

    //根据参数决定返回值类型
    //sync call
    template<size_t TIMEOUT, typename T = void, typename... Args>  //T为void情况下走此版本 返回值void
    //typename声明后面是一个类型；std::enable_if<A,B>,如果A成立，走偏特化版本，为B；否则此模板被丢弃(第二个参数默认为void)
    typename std::enable_if<std::is_void<T>::value>::type call(const std::string& rpc_name, Args&& ...args){
        auto future_result = async_call<FUTURE>(rpc_name, std::forward<Args>(args)...); //参数是rpc方法名称和传入参数
        auto status = future_result.wait_for(std::chrono::milliseconds(TIMEOUT));
        if(status == std::future_status::timeout || status == std::future_status::deferred){
            throw std::out_of_range("timeout or deferred");
        }
        future_result.get().as();
    }

    template<typename T = void, typename... Args> //T为void 情况下走此版本 返回值void
    typename std::enable_if<std::is_void<T>::value>::type call(const std::string& rpc_name, Args&& ...args){
        call<DEFAULT_TIMEOUT, T>(rpc_name, std::forward<Args>(args)...);
    }

    template<size_t TIMEOUT, typename T , typename... Args> //T不为void走此版本， 返回值类型T
    typename std::enable_if<!std::is_void<T>::value, T>::type call(const std::string& rpc_name, Args&& ...args){
        auto future_result = async_call<FUTURE>(rpc_name, std::forward<Args>(args)...);
        auto status = future_result.wait_for(std::chrono::milliseconds(TIMEOUT));
        if(status == std::future_status::timeout || status == std::future_status::deferred){
            throw std::out_of_range("timeout or deferred");
        }
        return future_result.get().template as<T>();
    }

    template<typename T, typename... Args>       //T不为空走此版本，返回值类型T
    typename std::enable_if<!std::is_void<T>::value, T>::type call(const std::string& rpc_name, Args&& ...args){
        return call<DEFAULT_TIMEOUT, T>(rpc_name, std::forward<Args>(args)...);
    }

    //future接口 将请求promise写入future_map_
    template<CallModel model, typename... Args> 
    //返回值是future_result结构体；参数分别是远程方法名称和传入参数（可变参数）
    future_result<req_result> async_call(const std::string& rpc_name, Args&&...args){
        auto p = std::make_shared<std::promise<req_result>>();
        std::future<req_result> future = p->get_future();

        uint64_t fu_id = 0;
        {
            std::unique_lock<std::mutex> lock(cb_mtx_);
            fu_id_++;
            fu_id = fu_id_;  //生成期望id
            future_map_.emplace(fu_id, std::move(p));//期望写入future_map_
        }

        msgpack_codec codec;
        auto ret = codec.pack_args(std::forward<Args>(args)...);        //参数打包
        //参数：请求id，请求类型，              序列化后的参数       函数名通过哈希算法计算出的函数id
        write(fu_id, request_type::req_res, std::move(ret), MD5::MD5Hash32(rpc_name.data()));  //请求写入
        //利用fu_id,future构建future_result结构体 作为返回值
        return future_result<req_result>{fu_id, std::move(future)};
    }

    //异步回调接口 参数多了一个回调函数 将回调函数写入callback_map_
    template<size_t TIMEOUT = DEFAULT_TIMEOUT, typename... Args>
    void async_call(const std::string& rpc_name, std::function<void(asio::error_code, string_view)> cb, Args... args){
        if(!has_connected_){
            if(cb) cb(asio::error::make_error_code(asio::error::not_connected),"not connected");
            return;
        }
        
        uint64_t cb_id = 0;
        {   
            std::unique_lock<std::mutex> lock(cb_mtx_);
            //生成回调函数id
            callback_id_++;
            callback_id_ |= (uint64_t(1)<<63);
            cb_id = callback_id_;            
            auto call = std::make_shared<call_t>(ios_, std::move(cb), TIMEOUT); //回调函数与定时器绑定
            call->start_timer();                //设置过期时间
            callback_map_.emplace(cb_id, call); //回调函数写入callback_map_
        }
        
        msgpack_codec codec;
        auto ret = codec.pack_args(std::forward<Args>(args)...);
        write(cb_id, request_type::req_res, std::move(ret), MD5::MD5Hash32(rpc_name.data()));
    }    

    void stop(){
        if(thd_!=nullptr){
            ios_.stop();
            if(thd_->joinable()) thd_->join();
            thd_ = nullptr;
        }
    }

    template<typename Func> 
    void subscribe(std::string key, Func f){
        auto it = sub_map_.find(key);
        if(it != sub_map_.end()){
            assert("duplicated subscribe");
            return;
        }

        sub_map_.emplace(key, std::move(f));
        send_subscribe(key, "");
        key_token_set_.emplace(std::move(key), "");
    }

    template<typename Func>
    void subscribe(std::string key, std::string token, Func f){
        auto composite_key = key + token;
        auto it = sub_map_.find(composite_key);
        if(it != sub_map_.end()){
            assert("duplicated subscribe");
            return;
        }

        sub_map_.emplace(std::move(composite_key), std::move(f));
        send_subscribe(key, token);
        key_token_set_.emplace(std::move(key), std::move(token));
    }

    template<typename T, size_t TIMEOUT = 3>
    void publish(std::string key, T&& t){
        msgpack_codec codec;
        auto buf = codec.pack(std::move(t));
        call<TIMEOUT>("publish", std::move(key), "", std::string(buf.data(), buf.size()));
    }

    template<typename T, size_t TIMEOUT = 3>
    void publish_by_token(std::string key, std::string token, T&& t){
        msgpack_codec codec;
        auto buf = codec.pack(std::move(t));
        call<TIMEOUT>("publish_by_token", std::move(key), std::move(token), std::string(buf.data(), buf.size()));
    }

#ifdef CINATRA_ENABLE_SSL
    void set_ssl_context_callback(std::function<void(asio::ssl::context&)> ssl_context_callback){
        ssl_context_callback_ = std::move(ssl_context_callback);
    }
#endif

private:
    //与服务端建立连接 异步执行 信号量进行通信
    void async_connect(){
        assert(port_ != 0);
        auto addr = asio::ip::address::from_string(host_);
        //与服务端建立连接 
        socket_.async_connect({addr,port_},[this](const asio::error_code& ec){
            if(has_connected_) return;
            if(ec){
                has_connected_ = false;
                if(reconnect_cnt_ <= 0) return;

                if(reconnect_cnt_ > 0) reconnect_cnt_--;

                async_reconnect();
            }else{
                if(is_ssl()){
                    handshake();
                    return;
                }

                has_connected_ = true;
                do_read();         //处理回调和ruture接口
                resend_subscribe();  //处理订阅-发布者模式
                if(has_wait_) conn_cond_.notify_one();  //信号量通知等待线程
            }
        });
    }

    void async_reconnect(){
        reset_socket();
        async_connect();
        std::this_thread::sleep_for(std::chrono::milliseconds(connect_timeout_));
    }

    void reset_deadline_timer(size_t timeout){
        deadline_.expires_from_now(std::chrono::seconds(timeout));
        deadline_.async_wait([this,timeout](const asio::error_code& ec){
            if(!ec){
                if(has_connected_) write(0, request_type::req_res, buffer_type(0), 0);
            }

            reset_deadline_timer(timeout);
        });
    }

    //带参write将请求写入输出队列outbox_
    void write(std::uint64_t req_id, request_type type, buffer_type&& message, uint32_t func_id){
        size_t size = message.size();
        assert(size<MAX_BUF_LEN);
        //利用{message.release(),size}构建string_view，参数分别是字符串的指针地址和字符串长度，并未进行拷贝
        client_message_type msg{req_id, type, {message.release(),size}, func_id};

        std::unique_lock<std::mutex> lock(write_mtx_);
        outbox_.emplace_back(std::move(msg));            //写入消息输出队列
        if(outbox_.size()>1) return;

        write();
    }

    void write(){       //将输出队列中的请求写入缓冲区并进行异步发送
        auto& msg = outbox_[0];
        write_size_ = (uint32_t)msg.content.length();
        std::array<asio::const_buffer, 2> write_buffers;
        header_ = {MAGIC_NUM, msg.req_type, write_size_, msg.req_id, msg.func_id};
        write_buffers[0] = asio::buffer(&header_, sizeof(rpc_header));  //写入缓冲区
        write_buffers[1] = asio::buffer((char*)msg.content.data(), write_size_);

        //调用async_write发送到服务端
        async_write(write_buffers, [this](const asio::error_code& ec, const size_t length){
            if(ec){
                has_connected_ = false;
                close(false);
                error_callback(ec);
                return;
            }

            std::unique_lock<std::mutex> lock(write_mtx_);

            if(outbox_.empty()) return;
            ::free((char*)outbox_.front().content.data());  //::代表调用全局函数，不是类自己的成员函数
            outbox_.pop_front();

            if(!outbox_.empty()) this->write();  //循环将outbox队列中的请求写入缓冲区
        });
    }

    void do_read(){
        async_read_head([this](const asio::error_code& ec, const size_t length){
            if(!socket_.is_open()){
                has_connected_ =false;
                return;
            }

            if(!ec){
                rpc_header* header = (rpc_header*)(head_);  //定义rpc头部指针接受即将读取的头部信息
                const uint32_t body_len = header->body_len;
                if(body_len>0 && body_len<MAX_BUF_LEN) {
                    if(body_.size() <body_len) body_.resize(body_len);  //防止body_装不下
                    //从读取的头部获取响应请求id、请求类型、响应体长度
                    read_body(header->req_id, header->req_type, body_len); 
                    return;
                }

                if(body_len == 0 || body_len>MAX_BUF_LEN){
                    close();
                    error_callback(asio::error::make_error_code(asio::error::message_size));
                    return;
                }
            }else{
                std::cout<<ec.message()<<"\n";
                {
                    std::unique_lock<std::mutex> lock(cb_mtx_);
                    for(auto& item:callback_map_) item.second->callback(ec,{});
                }
                close(false);
                error_callback(ec);
            }
        });
    }

    void read_body(std::uint64_t req_id, request_type req_type, size_t body_len){
        async_read(body_len, [this, req_id, req_type, body_len](asio::error_code ec, std::size_t length){
            if(!socket_.is_open()){
                call_back(req_id, asio::error::make_error_code(asio::error::connection_aborted),{});
                return;
            }
            if(!ec){
                if(req_type == request_type::req_res){
                    //给回调函数是没有反序列化的数据
                    call_back(req_id, ec, {body_.data(), body_len});  //vector<T>.data() 返回内置vector所指向数组内存的第一个元素的指针
                }else if(req_type == request_type::sub_pub){
                    callback_sub(ec, {body_.data(), body_len});
                }else{
                    close();
                    error_callback(asio::error::make_error_code(asio::error::invalid_argument));
                    return;
                }
                do_read();         //建立一个循环
            }else{
                has_connected_ = false;
                call_back(req_id, ec, {});
                close();
                error_callback(ec);
            }
        });
    }

    //发送订阅
    void send_subscribe(const std::string& key, const std::string& token){
        msgpack_codec codec;
        auto ret = codec.pack_args(key, token);
        write(0, request_type::sub_pub, std::move(ret), MD5::MD5Hash32(key.data()));
    }

    void resend_subscribe(){
        if(key_token_set_.empty()) return;

        for(auto& pair:key_token_set_) send_subscribe(pair.first, pair.second);
    }

    //读取响应体之后执行回调 或者 给promise接口set_value让其返回值
    //传入回调函数中的data，以及给promise的值都没有反序列化
    //回调函数反序列化操作在回调函数中自行实现； future接口反序列化操作在req_result结构中get_result()函数中实现
    void call_back(uint64_t req_id, const asio::error_code& ec, string_view data){
        temp_req_id_ = req_id;
        auto cb_flag = req_id>>63;
        if(cb_flag){
            std::shared_ptr<call_t> cl = nullptr;
            {
                std::unique_lock<std::mutex> lock(cb_mtx_);
                cl = std::move(callback_map_[req_id]);
            }
            assert(cl);
            if(!cl->has_timeout()){
                cl->cancel();
                cl->callback(ec, data);
            }else{
                cl->callback(asio::error::make_error_code(asio::error::timed_out),{});
            }
            
            std::unique_lock<std::mutex> lock(cb_mtx_);
            callback_map_.erase(req_id);
        }else{
            std::unique_lock<std::mutex> lock(cb_mtx_);
            auto& f = future_map_[req_id];
            if(ec){
                if(f){
                    f->set_value(req_result{""});  //这里的数据还没有反序列化，反序列化在主函数中调用.as()执行
                    future_map_.erase(req_id);
                    return;
                }
            }
            assert(f);
            //promise.set_value() 原子存储value进入共享状态并使得状态就绪(ready)
            f->set_value(req_result{data});  //将结果data写入req_result，反序列化在req_result中的get_result()实现
            future_map_.erase(req_id);
        }
    }

    void callback_sub(const asio::error_code& ec, string_view result){
        rpc_service::msgpack_codec codec;
        try{
            auto tp = codec.unpack<std::tuple<int, std::string, std::string>>(result.data(), result.size());
            auto code = std::get<0> (tp);    //ok
            auto& key = std::get<1>(tp);     //key
            auto& data = std::get<2>(tp);    //data

            auto it = sub_map_.find(key);
            if(it == sub_map_.end()) return;

            it->second(data);
        }catch(const std::exception&){
            error_callback(asio::error::make_error_code(asio::error::invalid_argument));
        }
    }

    void clear_cache(){
        {
            std::unique_lock<std::mutex> lock(write_mtx_);
            while(!outbox_.empty()){
                ::free((char*)outbox_.front().content.data());
                outbox_.pop_front();
            }
        }

        {
            std::unique_lock<std::mutex> lock(cb_mtx_);
            callback_map_.clear();
            future_map_.clear();
        }
    }

    void reset_socket(){
        asio::error_code igored_ec;
        socket_.close(igored_ec);
        socket_ = decltype(socket_)(ios_);
        if(!socket_.is_open()) socket_.open(asio::ip::tcp::v4());
    }

    //将回调函数和定时器绑定
    class call_t: asio::noncopyable, public std::enable_shared_from_this<call_t>{
        public:
            call_t(asio::io_service& ios, std::function<void(asio::error_code, string_view)> cb, size_t timeout)
                :timer_(ios), cb_(std::move(cb)), timeout_(timeout){}

        void start_timer(){
            if(timeout_ ==0)return;
            //定时器设置过期时间
            timer_.expires_from_now(std::chrono::milliseconds(timeout_));
            auto self = this->shared_from_this();
            timer_.async_wait([this,self](asio::error_code ec){
                if(ec) return;

                has_timeout_ = true;
            });
        }

        void callback(asio::error_code ec, string_view data){cb_(ec, data);}

        bool has_timeout() const {return has_timeout_;}

        void cancel(){
            if(timeout_ ==0) return;

            asio::error_code ec;
            timer_.cancel(ec);
        }

        private:
            asio::steady_timer timer_;
            std::function<void(asio::error_code, string_view)> cb_;
            size_t timeout_;
            bool has_timeout_ = false;  
    };

    void error_callback(const asio::error_code& ec){
        if(err_cb_) err_cb_(ec);
        if(enable_reconnect_) async_reconnect();
    }

    void set_default_error_cb(){
        err_cb_ = [this](asio::error_code){async_connect();};
    }

    bool is_ssl() const{
#ifdef CINATRA_ENABLE_SSL
        return ssl_stream_ != nullptr;
#else
        return false;
#endif
    }

    void handshake(){
#ifdef CINATRA_ENABLE_SSL
        ssl_stream_->async_handshake(asio::ssl::stream_base::client,[this](const asio::error_code& ec){
            if(!ec){
                has_connected_ = true;
                do_read();
                resend_subscribe();
                if(has_wait_) conn_cond_.notify_one();
                else{
                    error_callback(ec);
                    close();
                }
            }
        });
#endif
    }

    void upgrade_to_ssl(){
#ifdef CINATRA_ENABLE_SSL
        if(ssl_stream_) return;

        asio::ssl::context ssl_context(asio::ssl::context::sslv23);
        ssl_context.set_default_verify_paths();
        asio::error_code ec;
        ssl_context.set_options(asio::ssl::context::default_workarounds, ec);
        if(ssl_contextcallback_){
            ssl_context_callback_(ssl_context);
        }
        ssl_stream_ = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket&>>(socket_, ssl_context);
#else
        assert(is_ssl()); 
#endif
    }

    template<typename Handler>
    void async_read_head(Handler handler){
        if(is_ssl()){
#ifdef CINATRA_ENABLE_SSL
            asio::async_read(*ssl_stream_, asio::buffer(head_, HEAD_LEN),std::move(handler));
#endif
        }else{       //异步将数据读取size个字节数进入asio::buffer缓冲区
            asio::async_read(socket_, asio::buffer(head_, HEAD_LEN), std::move(handler));
        }
    }

    template<typename Handler>
    void async_read(size_t size_to_read, Handler handler){  //读取到body_中
        if(is_ssl()){
#ifdef CINATRA_ENABLE_SSL
            asio::async_read(*ssl_stream_, asio::buffer(body_.data(),size_to_read),std::move(handler));
#endif
        }else{
            asio::async_read(socket_, asio::buffer(body_.data(),size_to_read),std::move(handler));
        }
    }

    //通过这里发送给服务端
    template<typename BufferType, typename Handler>
    void async_write(const BufferType& buffers, Handler handler){
        if(is_ssl()){
#ifdef CINATRA_ENABLE_SSL
            asio::async_write(*ssl_stream_, buffers, std::move(handler));
#endif
        }else{
            asio::async_write(socket_, buffers, std::move(handler));  //发送
        }
    }

    ZookeeperClient zkClient_; //增加zookeeper客户端来查询

    asio::io_context ios_;
    asio::ip::tcp::socket socket_;
#ifdef CINATRA_ENABLE_SSL
    std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> ssl_stream_;
    std::function<void(asio::ssl::context&)> ssl_context_callback_;
#endif
    asio::io_context::work work_;
    std::shared_ptr<std::thread> thd_ = nullptr;

    ZookeeperClient zk_client_;       //新增Zookeeper客户端 用于查询远程服务的ip地址和端口

    std::string host_;
    unsigned short port_ = 0;
    size_t connect_timeout_ = 1000; //s
    int reconnect_cnt_ = -1;
    std::atomic_bool has_connected_ ={false};
    std::mutex conn_mtx_;
    std::condition_variable conn_cond_;
    bool has_wait_ = false;

    asio::steady_timer deadline_; //定时器

    struct client_message_type{    //客户端消息结构体
        std::uint64_t req_id;     //请求id
        request_type req_type;    //请求类型
        //用c++11编译需要将std去掉，此时标准命名空间还没有string_view
        //std::string_view content;  //序列化后的参数
        string_view content;  //序列化后的参数
        uint32_t func_id;          //函数名通过哈希算法计算出的函数id
    };

    std::deque<client_message_type> outbox_; //输出队列
    uint32_t write_size_ = 0;
    std::mutex write_mtx_;
    uint64_t fu_id_ = 0;
    std::function<void(asio::error_code)> err_cb_;
    bool enable_reconnect_ = false;

    std::unordered_map<std::uint64_t, std::shared_ptr<std::promise<req_result>>> future_map_;   //future方法

    std::unordered_map<std::uint64_t, std::shared_ptr<call_t>> callback_map_;  //回调方法

    std::mutex cb_mtx_;
    uint64_t callback_id_ = 0;
    uint64_t temp_req_id_ = 0;

    char head_[HEAD_LEN] = {};

    std::vector<char> body_;
    rpc_header header_;
    //使用c++11编译 此时标准命名空间还没有string_view
    //std::unordered_map<std::string, std::function<void(std::string_view)>> sub_map_;
    std::unordered_map<std::string, std::function<void(string_view)>> sub_map_;
    std::set<std::pair<std::string, std::string>> key_token_set_;

    std::function<void(long, const std::string&)> on_result_received_callback_;

};

}//namespace rest_rpc