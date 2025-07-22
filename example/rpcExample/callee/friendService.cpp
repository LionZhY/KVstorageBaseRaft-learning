#include <mprpcchannel.h>
#include <iostream>
#include "rpcExample/friend.pb.h"
#include "rpcprovider.h" // 自定义的 RPC 框架类，负责发布服务并运行服务节点
#include <vector>
#include <string>


/* 实现了一个 RPC 服务端节点，用于响应 “查询好友列表” 的 RPC 请求 */


// 服务类 FriendService
class FriendService : public fixbug::FiendServiceRpc  // 继承自 friend.proto 生成的 FiendServiceRpc，是服务端实现类
{
public:
	//  本地业务方法（非 RPC 框架调用，实际逻辑）
	std::vector<std::string> GetFriendsList(uint32_t userid) 
	{
		/* 模拟从数据库或业务逻辑中获取好友列表。这里只是返回了固定的好友名字*/
		std::cout << "local do GetFriendsList service! userid:" << userid << std::endl;
		std::vector<std::string> vec;
		vec.push_back("gao yang");
		vec.push_back("liu hong");
		vec.push_back("wang shuo");
		return vec;
	}

	// 重写 RPC 基类方法 (RPC 入口)
	void GetFriendsList(::google::protobuf::RpcController *controller, 
						const ::fixbug::GetFriendsListRequest *request,
	                    ::fixbug::GetFriendsListResponse *response, 
						::google::protobuf::Closure *done) 
	{
		/* 这是 RPC 框架自动生成的服务函数接口，用户需要实现它 */

		uint32_t userid = request->userid(); // 解析参数

		// 调用本地业务逻辑
		std::vector<std::string> friendsList = GetFriendsList(userid); 

		// 组装响应
		response->mutable_result()->set_errcode(0);
		response->mutable_result()->set_errmsg("");
		for (std::string &name : friendsList) 
		{
			std::string *p = response->add_friends();
			*p = name;
		}

		done->Run(); // 触发回调函数 done，通知 RPC 框架当前方法执行完毕，进行序列化并返回给客户端
	}
};



// main() 函数 用于启动 RPC 服务节点
int main(int argc, char **argv) 
{
	// 创建客户端 stub（此处未用上） 只是示例代码
	/* Client Stub 是用来发起请求调用别的节点的，实际服务端如果不用去调用别的节点，就不需要主动创建 Client Stub */
	std::string ip = "127.0.0.1";
	short port = 7788;
	auto stub = new fixbug::FiendServiceRpc_Stub(new MprpcChannel(ip, port, false));
	

	// 创建 RpcProvider 并注册服务：provider是一个rpc网络服务对象。把 FriendService 注册到框架上
	RpcProvider provider;
	provider.NotifyService(new FriendService());


	// 启动一个rpc服务节点  启动服务监听，阻塞等待远程客户端的 RPC 请求
	provider.Run(1, 7788); // (节点编号，服务监听端口)


  	return 0;
}