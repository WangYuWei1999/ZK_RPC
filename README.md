# 分布式网络通信框架ZK_RPC
一、项目介绍：基于TCP的高性能,易用,跨平台,加密通信的RPC分布式网络通信框架项目,实现了同一台服务器的不同进程之间或不同服务器之间的远程服务调用功能。
二、应用技术：Linux、Boost、ZooKeeper、MessagePack、SSL、MD5
1.使用ZooKeeper注册服务端本地方法和查询客户端远程方法地址,客户端连接服务端不必手动输入IP和端口信息,实现远程调用自动化；
2.使用MessagePack实现传输数据序列化以及反序列化,使用SSL加密防止传输数据被窃听或更改,使用MD5Hash32算法生成唯一函数ID用于注册和调用；
3.使用Boost:asio库以及线程池处理异步io事件,实现高并发底层网络通信；
4.客户端接口丰富,分别实现了带超时的异步回调接口和future接口,提供了订阅-发布者模式接口；
5.实现基于无锁队列的异步Logger日志模块记录框架运行信息；