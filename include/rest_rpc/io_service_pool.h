#ifndef REST_RPC_IO_SERVICE_POOL_H_
#define REST_RPC_IO_SERVICE_POOL_H_

#include"use_asio.hpp"
#include<memory>
#include<vector>

//线程池
//io_service对象是asio框架中的调度器，
//所有异步io事件都是通过它来分发处理的（io对象的构造函数中都需要传入一个io_service对象）
namespace rest_rpc{
namespace rpc_service{
class io_service_pool:private asio::noncopyable{
public:
    //explicit 指定构造函数或转换函数 (C++11起)为显式, 即它不能用于隐式转换和复制初始化.
    explicit io_service_pool(std::size_t pool_size):next_io_service_(0){
        if(pool_size==0) throw std::runtime_error("io_service_pool size is 0");
        for(std::size_t i=0;i<pool_size;++i){
            io_service_ptr io_service(new asio::io_context);
            work_ptr work(new asio::io_context::work(*io_service));
            io_services_.push_back(io_service);
            work_.push_back(work);
        }
    }

    //多个io_context对应多个线程(每个线程都有一个io_context，调用各自的run方法)
    void run(){//创建线程池
        std::vector<std::shared_ptr<std::thread>> threads;
        for(std::size_t i=0;i<io_services_.size();++i){      //调用run()函数进入io事件循环
            //lambda表达式包装io_service.run()函数作为线程函数，创建线程池
            threads.emplace_back(std::make_shared<std::thread>([](io_service_ptr svr){svr->run();},io_services_[i]));
        }
        for(std::size_t i = 0; i<threads.size(); ++i) threads[i]->join();
    }

    void stop(){
        for(std::size_t i=0; i<io_services_.size();++i) io_services_[i]->stop();
    }

    //分配线程
    asio::io_context& get_io_service(){
        asio::io_context& io_service = *io_services_[next_io_service_];
        ++next_io_service_;
        if(next_io_service_ == io_services_.size()) next_io_service_ = 0;
        return io_service;  //返回一个asio::io_context
    }   

private:
    typedef std::shared_ptr<asio::io_context> io_service_ptr;  //asio::io_context表示io的上下文
    typedef std::shared_ptr<asio::io_context::work> work_ptr;  //asio::io_context::work可以防止io_context在没有io事件的情退出

    std::vector<io_service_ptr> io_services_; 

    std::vector<work_ptr> work_;
    std::size_t next_io_service_;
};
}
}

#endif