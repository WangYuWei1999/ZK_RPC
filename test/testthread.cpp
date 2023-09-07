// // #include<thread>
// // #include<iostream>

// // int main(){
// //     std::cout<<std::thread::hardware_concurrency()<<std::endl;
// //     return 0;
// // }

// #include <iostream>
// #include <boost/asio.hpp>

// int get_localip(const char * eth_name, char *local_ip_addr)
// {
// 	int ret = -1;
//     int fd;
//     struct ifreq ifr;
 
// 	if (local_ip_addr == NULL || eth_name == NULL)
// 	{
//         std::cout<<"aaa"<<std::endl;
// 		return ret;
// 	}
// 	if ((fd=socket(AF_INET, SOCK_DGRAM, 0)) > 0)
// 	{
// 		strcpy(ifr.ifr_name, eth_name);
// 		if (!(ioctl(fd, SIOCGIFADDR, &ifr)))
// 		{
// 			ret = 0;
// 			strcpy(local_ip_addr, inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));
// 		}
// 	}
// 	if (fd > 0)
// 	{
// 		close(fd);
// 	}
//     return ret;
// }

// int main(void) {

//     std::cout<<"thread numbers: "<<std::thread::hardware_concurrency()<<std::endl;
//     std::cout << "server start." << std::endl;
//     char local_ip[20] = {0};
//     int ret = get_localip("eth0", local_ip);
//     if (ret == 0)
// 	{
// 		printf("local ip:%s\n", local_ip);
// 	}
// 	else
// 	{
// 		printf("get local ip failure\n");
// 	}

//     // asio程序必须的io_service对象
//     boost::asio::io_service ios;
//     // 具体的服务器地址与端口
//     boost::asio::ip::tcp::endpoint endpotion(boost::asio::ip::tcp::v4(), 13695);
//     // 创建acceptor对象，当前的IPV4作为服务器地址(127.0.0.1 || 0.0.0.0)，接受端口13695的消息.
//     boost::asio::ip::tcp::acceptor acceptor(ios, endpotion);
//     boost::asio::ip::address addr = acceptor.local_endpoint().address();
//     std::string ip = acceptor.local_endpoint().address().to_string();
//     // 打印当前服务器地址
//     std::cout << "addr: " << acceptor.local_endpoint().address() << std::endl;
//     std::cout << "addr: " << ip << std::endl;
//     // 打印当前服务器端口
//     std::cout << "port: " << acceptor.local_endpoint().port() << std::endl;
// 	return 0;
// }

#include<iostream>
#include<vector>
using namespace std;
#include<math.h>

int main() {
	int n;
	int input;
	cin >> n;
	vector<int> start_time;
	vector<int> cost_time;
	vector<int> finish_time;
	vector<int> value;
	//vector<vector<int>> dp(n,{0});
	int dp[1000][1000];

	for (int i = 0; i < n; ++i) {
		cin >> input;
		start_time.push_back(input);
	}

	for (int i = 0; i < n; ++i) {
		cin >> input;
		cost_time.push_back(input);
	}

	for (int i = 0; i < n; ++i) {
		cin >> input;
		value.push_back(input);
	}

	for (int i = 0; i < n; ++i) {
		finish_time.push_back(start_time[i] + cost_time[i]);
	}

	for (int i = 1; i <= n; ++i) {
		for (auto& t: finish_time) {
			
			if (t <= start_time[i-1]) dp[i][t] = max(dp[i - 1][t] + value[i], dp[i - 1][finish_time[i - 1]]);
			else dp[i][t] = dp[i - 1][finish_time[i - 1]];
		}
	}

	cout << dp[n-1][finish_time[n-1]]<<endl;
	return 0;
}

