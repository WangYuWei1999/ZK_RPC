#ifndef REST_RPC_RPC_SERVER_H_
#define REST_RPC_RPC_SERVER_H_
#include"connection.h"
#include"io_service_pool.h"
#include"router.h"
#include<condition_variable>
#include<mutex>
#include<thread>
#include <zookeeper/zookeeper.h>
#include<string.h>
#include"zookeeperclient.hpp"

using asio::ip::tcp;

namespace rest_rpc{
namespace rpc_service{
using rpc_conn = std::weak_ptr<connection>;
//noncopyable私有化类的拷贝构造函数和拷贝赋值操作符，这样子类可以调用，但是外部调用者不能通过复制/赋值等语句来产生一个新的对象
class rpc_server:private asio::noncopyable{
public:             //变化1,构造函数增加一个参数：服务对象名称
    rpc_server(std::string servervicename, unsigned short port, size_t size, size_t timeout_seconds = 15, size_t check_seconds = 10) 
    :port_(port), service_name_(servervicename), ser_path_("/"+servervicename), io_service_pool_(size),acceptor_(io_service_pool_.get_io_service(), tcp::endpoint(tcp::v4(),port)),
    timeout_seconds_(timeout_seconds), check_seconds_(check_seconds), signals_(io_service_pool_.get_io_service()){  //利用io_service即io_context初始化
        
        ip_ = RpcConfigure::get_configure().find("rpcserver_ip");  //利用RpcConfigure单例从本地文件rpc.conf查询ip
        std::cout<<"ip_: "<<ip_<<std::endl;
        char ip_port[128]={0};
        sprintf(ip_port,"%s:%d", ip_.c_str(), port_);
        //在zookeeper中创建一个根节点 节点值为ip地址+端口
        zk_client_.create(ser_path_.c_str(), ip_port, strlen(ip_port));
        //zk_client_.create(ser_path_.c_str(), ip_.c_str(), ip_.size());

        do_accept();
        check_thread_ = std::make_shared<std::thread>([this]{clean();});
        pub_sub_thread_ = std::make_shared<std::thread> ([this]{clean_sub_pub();});
        signals_.add(SIGINT);  //2
        signals_.add(SIGTERM); //15
#if defined(SIGQUIT)
        signals_.add(SIGQUIT);
#endif
        do_await_stop();
    }
                    //构造函数增加参数服务名称
    rpc_server(std::string servervicename, unsigned short port, size_t size, ssl_configure ssl_conf, size_t timeout_seconds = 15, size_t check_seconds = 10)
        :rpc_server(servervicename, port, size, timeout_seconds, check_seconds){
#ifdef CINATRA_ENABLE_SSL
        ssl_conf_ = std::move(ssl_conf);
#else   
        assert(false);// please add definition CINATRA_ENABLE_SSL, not allowed
                   // coming in this branch
#endif
        }

        ~rpc_server(){stop();}        

        void async_run(){  //创建线程池
            thd_ = std::make_shared<std::thread>([this]{io_service_pool_.run();});
        }

        void run(){io_service_pool_.run();}

        //服务端注册函数 默认同步 注册非成员函数
        template<ExecMode model = ExecMode::sync, typename Function>
        void register_handler(const std::string& name, const Function& f){
            //创建zk节点 ZOO_EPHEMERAL表示临时节点  变化2
            zk_client_.create_method(ser_path_, name, ip_, port_, ZOO_EPHEMERAL);            
            router_.register_handler<model>(name, f);
        }

        //注册成员函数 默认同步
        template<ExecMode model = ExecMode::sync, typename Function, typename Self>
        void register_handler(const std::string& name, const Function& f, Self*self){
            zk_client_.create_method(ser_path_, name, ip_, port_, ZOO_EPHEMERAL);    //将成员函数注册到zk上
            router_.register_handler<model>(name, f, self);
        }

        void set_conn_timeout_callback(std::function<void(int64_t)> callback){
            conn_timeout_callback_ = std::move(callback);
        }

        void set_network_err_callback(std::function<void(std::shared_ptr<connection>, std::string)> on_net_err){
            on_net_err_callback_ = std::move(on_net_err);
        }

        template<typename T> void publish(const std::string& key, T data){
            publish(key, "", std::move(data));
        }

        template<typename T>
        void publish_by_token(const std::string& key, std::string token, T data){
            publish(key, std::move(token), std::move(data));
        }

        std::set<std::string> get_token_list(){
            std::unique_lock<std::mutex> lock(sub_mtx_);
            return token_list_;
        }

private:

    void do_accept(){   //利用io_service_pool_.get_io_service()分配一个io_context，和roter对象创建新连接
        conn_.reset(new connection(io_service_pool_.get_io_service(),timeout_seconds_, router_));
        conn_->set_callback([this](std::string key, std::string token, std::weak_ptr<connection> conn){
            std::unique_lock<std::mutex> lock(sub_mtx_);
            sub_map_.emplace(std::move(key)+token, conn);  //连接进入哈希表，给发布者模式用来发布
            if(!token.empty()){
                token_list_.emplace(std::move(token));
            }
        });
        //等到server.run（）时候才运行
        //io_context将操作的结果取出队列，将其转换为错误代码，然后将其传递给完成处理程序（回调函数）
        acceptor_.async_accept(conn_->socket(),[this](asio::error_code ec){
            if(!acceptor_.is_open()) return;
            if(ec){}
            else{
#ifdef CINATRA_ENABLE_SSL
            if(!ssl_conf_.cert_file.empty()) conn_->init_ssl_context(ssl_conf_);
#endif
            if(on_net_err_callback_) conn_->on_network_error(on_net_err_callback_); //将on_net_err_callback_穿过去
            conn_->start();
            std::unique_lock<std::mutex> lock(mtx_);
            conn_->set_conn_id(conn_id_);
            connections_.emplace(conn_id_++, conn_);
            }
            do_accept(); //递归
        });
    }

    void clean(){
        while(!stop_check_){
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait_for(lock, std::chrono::seconds(check_seconds_));
                                        //cbegin() const类型迭代器
            for(auto it = connections_.cbegin(); it!= connections_.cend();){
                if(it->second->has_closed()){
                    if(conn_timeout_callback_) conn_timeout_callback_(it->second->conn_id());

                    it = connections_.erase(it);
                }else it++;
            }
        }
    }

    void clean_sub_pub(){
        while(!stop_check_pub_sub_){
            std::unique_lock<std::mutex> lock(sub_mtx_);
            sub_cv_.wait_for(lock, std::chrono::seconds(10));

            for(auto it = sub_map_.cbegin(); it != sub_map_.cend();){
                auto conn = it->second.lock(); //weak_ptr.lock() 返回shared_ptr
                if(conn == nullptr || conn->has_closed()){
                    for(auto t = token_list_.begin(); t!=token_list_.end();){
                        if(it->first.find(*t) != std::string::npos) t = token_list_.erase(t); //npos 表示不存在的位置
                        else ++t;
                    }
                    it = sub_map_.erase(it);    //erase的返回值是一个迭代器，指向删除元素下一个元素
                }else ++it;
            }
        }
    }

    //订阅者模式使用weak_ptr 发布者不能管理订阅者生命周期，若订阅者存在（连接connection存在）向其发布消息
    template<typename T>
    void publish(std::string key, std::string token, T data){
        {
            std::unique_lock<std::mutex> lock(sub_mtx_);
            if(sub_map_.empty()) return;
        }
        std::shared_ptr<std::string> shared_data = get_shared_data<T>(std::move(data));
        std::unique_lock<std::mutex> lock(sub_mtx_);
        //返回pair<iterator,iterator>
        auto range = sub_map_.equal_range(key+token); //返回从key+token到最后迭代器
        for(auto it = range.first; it!=range.second; ++it){
        //使用weak_ptr 发布者不能管理订阅者生命周期，若订阅者存在（连接connection存在）向其发布消息
            auto conn = it->second.lock();      //weak_ptr() 转变为shared_ptr()
            if(conn == nullptr || conn->has_closed()) continue;
            conn->publish(key+token,*shared_data);
        }
    }
    
    template<typename T>   //is_assignable 检查能否将string类型的值赋给T
    typename std::enable_if<std::is_assignable<std::string, T>::value, std::shared_ptr<std::string>>::type
    get_shared_data(std::string data){
        return std::make_shared<std::string> (std::move(data));
    }

    template<typename T>   //is_assignable<T,U>::value 如果T可以从U赋值，value为true否则false；
    //enable_if<A,B>::type 如果A为true,则tyep是B，否则不执行
    //如果类型T可以赋值给string,value为真，enable_if条件成立，type类型为shared_ptr<std::string>
    typename std::enable_if<!std::is_assignable<std::string, T>::value, std::shared_ptr<std::string>>::type
    get_shared_data(T data){
        msgpack_codec codec;
        auto buf = codec.pack(std::move(data));
        return std::make_shared<std::string> (buf.data(),buf.size());
    }

    void do_await_stop(){
        signals_.async_wait([this](std::error_code, int){stop();}); //等待信号触发，执行函数
    }

    void stop(){
        if(has_stoped_) return;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            stop_check_ = true;
            cv_.notify_all();
        }
        check_thread_->join();
        {
            std::unique_lock<std::mutex> lock(sub_mtx_);
            stop_check_pub_sub_ = true;
            sub_cv_.notify_all();
        }
        pub_sub_thread_->join();

        io_service_pool_.stop();
        if(thd_) thd_->join();
        has_stoped_ = true;
    }

    std::string ip_;                 //新增本机ip
    uint16_t port_;                  //新增本机端口号
    std::string service_name_;       //新增服务对象名称，用于在zk上注册根节点
    std::string ser_path_;           //新增服务对象根节点路径：/service_name_
    ZookeeperClient zk_client_;      //新增Zookeeper客户端 用于注册本地服务

    io_service_pool io_service_pool_; //线程池
    tcp::acceptor acceptor_;
    std::shared_ptr<connection> conn_;   //连接指针
    std::shared_ptr<std::thread> thd_;

    std::size_t timeout_seconds_;    //connection连接过期时间
    std::unordered_map<int64_t, std::shared_ptr<connection>> connections_; //连接编号和连接
    int64_t conn_id_ = 0; //连接编号
    std::mutex mtx_ ;
    std::shared_ptr<std::thread> check_thread_;
    size_t check_seconds_;
    bool stop_check_ = false;
    std::condition_variable cv_;  //条件变量
    
    asio::signal_set signals_;   //提供信号,触发信号，执行函数
    std::function<void(int64_t)> conn_timeout_callback_;  //超时回调函数
    std::function<void(std::shared_ptr<connection>, std::string)> on_net_err_callback_ = nullptr; //网络回调
    std::unordered_multimap<std::string, std::weak_ptr<connection>> sub_map_; ////订阅者模式使用std::weak_ptr<connection>
    std::set<std::string> token_list_;
    std::mutex sub_mtx_;
    std::condition_variable sub_cv_;
    std::shared_ptr<std::thread> pub_sub_thread_;
    bool stop_check_pub_sub_ = false;
    ssl_configure ssl_conf_;
    router router_;
    std::atomic_bool has_stoped_ = {false};
};

}  //namespace rpc_service
}  //namespace rest_rpc

#endif