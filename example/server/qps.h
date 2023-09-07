#pragma once
#include<atomic>
#include<chrono>
#include<thread>
#include<iostream>

class qps{
public:
    void increase(){counter_.fetch_add(1, std::memory_order_release);}

    qps():counter_(0){
        thd_ = std::thread([this]{
            while(!stop_){
                std::cout<<"qps: "<<counter_.load(std::memory_order_acquire)<<std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        });
    }

    ~qps(){
        stop_ = true;
        thd_.join();
    }

private:
    bool stop_ = false;
    std::thread thd_;
    std::atomic<uint32_t> counter_;
};