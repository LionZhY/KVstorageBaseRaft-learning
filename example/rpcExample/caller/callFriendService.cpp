#include <iostream>

// #include "mprpcapplication.h"
#include "rpcExample/friend.pb.h"

#include "mprpcchannel.h"
#include "mprpccontroller.h"
#include "rpcprovider.h"


/*  RPC 框架客户端调用示例：展示客户端如何通过 RPC 框架远程调用服务端的 GetFriendsList 方法 */


int main(int argc, char **argv) 
{

	// 设置远程服务的 ip 和 端口  
	std::string ip = "172.25.99.6"; // 127.0.1.1 在某些 Linux 环境中被用作主机别名地址
	short port = 7788; // 表示客户端将连接运行在本地 127.0.1.1:7788 的 RPC 服务（即 FriendService 服务端）
	

	// 创建 Client Stub 代理对象 （是通过 .proto 文件自动生成的 Stub 类，封装了服务调用接口）
	fixbug::FiendServiceRpc_Stub stub(new MprpcChannel(ip, port, true));  
									// 传入的 MprpcChannel 是用户自定义的通道类，封装了序列化、网络发送等底层逻辑
									// 传入 ip、port 是为了告诉通道要向哪台主机发起请求
									// true 代表使用长连接模式（即多次调用共用同一个 TCP 连接）													  
																		  
	
	// 构造rpc请求和响应对象
	fixbug::GetFriendsListRequest request;   
	request.set_userid(1000); // 设置要查询的用户 ID 为 1000
	fixbug::GetFriendsListResponse response; // 响应对象，接收服务器返回的好友列表


	// 调用远程服务（循环调用多次测试长连接）
	MprpcController controller;
	int count = 10;
	while (count--) // 长连接测试，发送10次请求
	{
	  	std::cout << " 倒数" << count << "次发起RPC请求" << std::endl;

		// 发起调用
	  	stub.GetFriendsList(&controller, &request, &response, nullptr); 
		/* 	stub.GetFriendsList(...) --> MprpcChannel::CallMethod(...) 核心入口
			所有 rpc 请求都经由 MprpcChannel::CallMethod() 实现底层封装，实际最终调用的都是 MprpcChannel::CallMethod() */
		

	  	
	  	/* rpc和业务本质上是隔离的，controller 负责框架级错误检测
		   rpc调用是否失败由框架来决定（rpc调用失败 ！= 业务逻辑返回false）*/
		
		// 一次rpc调用完成，处理调用的结果  （注意区分框架错误与业务错误）
		if (controller.Failed()) // RPC 框架通信失败（比如连接失败、网络中断等），controller 会标记失败状态
		{
			std::cout << controller.ErrorText() << std::endl;
		}
		else // 处理业务逻辑结果
		{
			if (0 == response.result().errcode()) // 业务逻辑正常，输出好友列表
			{
				std::cout << "rpc GetFriendsList response success!" << std::endl;
				int size = response.friends_size();
				for (int i = 0; i < size; i++) {
				  std::cout << "index:" << (i + 1) << " name:" << response.friends(i) << std::endl;
				}
			} 
			else // 业务失败
			{
				std::cout << "rpc GetFriendsList response error : " << response.result().errmsg() << std::endl;
			}
		}

	  	sleep(5);  // 每次调用之间暂停 5 秒，模拟 “稳定间隔调用” 的长连接调用场景，验证连接是否可复用
	}
	


	return 0;
}