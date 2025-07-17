#include <iostream>

// #include "mprpcapplication.h"
#include "rpcExample/friend.pb.h"

#include "mprpcchannel.h"
#include "mprpccontroller.h"
#include "rpcprovider.h"


/*  分布式 RPC 框架客户端调用示例，
	核心功能是：通过 MprpcChannel 自定义的网络通道，向远程 RPC 服务端（FriendService）发起 GetFriendsList 方法的调用 */


int main(int argc, char **argv) 
{
	// https://askubuntu.com/questions/754213/what-is-difference-between-localhost-address-127-0-0-1-and-127-0-1-1
	
	// 指定远程RPC服务的IP和端口
	std::string ip = "127.0.1.1"; // 127.0.1.1 在某些 Linux 环境中被用作主机别名地址
	short port = 7788;
	
	// 创建 Stub 代理对象 （是由 protobuf 编译器自动生成的 客户端Stub类，用于模拟远程服务调用）
	fixbug::FiendServiceRpc_Stub stub(new MprpcChannel(ip, port, true));  // 传入的 MprpcChannel 是用户自定义的通道类，封装了序列化、网络发送等底层逻辑。
																		  // 传入 ip、port 是为了告诉通道要向哪台主机发起请求
																		  // true 代表使用长连接模式（保持 TCP 连接不断开）
	
	// 填充请求值和返回值 构造rpc请求和响应对象
	fixbug::GetFriendsListRequest request;   
	request.set_userid(1000); // 设置要查询的用户 ID 为 1000
	fixbug::GetFriendsListResponse response; 


	// 多次发起 rpc 方法的调用
	// 消费者的stub最后都会调用到channel的 call_method方法  同步的rpc调用过程  MprpcChannel::callmethod
	MprpcController controller;
	int count = 10;
	while (count--) // 长连接测试，发送10次请求
	{
	  	std::cout << " 倒数" << count << "次发起RPC请求" << std::endl;
	  	stub.GetFriendsList(&controller, &request, &response, nullptr); // 发起调用

		// 实际最终调用的是 MprpcChannel::CallMethod()
	  	// RpcChannel->RpcChannel::callMethod 集中来做所有rpc方法调用的参数序列化和网络发送

	  	// 一次rpc调用完成，处理调用的结果
	  	// rpc和业务本质上是隔离的，controller 负责框架级错误检测
		// rpc调用是否失败由框架来决定（rpc调用失败 ！= 业务逻辑返回false）

		if (controller.Failed()) // 如果通信层失败（比如连接失败、网络中断等），controller 会标记失败状态
		{
			std::cout << controller.ErrorText() << std::endl;
		}
		else // 处理业务逻辑结果
		{
			if (0 == response.result().errcode()) // 业务成功
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

	  	sleep(5);  // 睡眠 5 秒后再次请求，模拟稳定的长连接调用场景，验证连接是否可复用
	}
	
	return 0;
}