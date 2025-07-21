#pragma once
// 引入 RPC 所需的 Protobuf 描述符相关类，Muduo 网络库的核心组件
#include "google/protobuf/service.h"
#include <functional>
#include <google/protobuf/descriptor.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/TcpConnection.h>
#include <muduo/net/TcpServer.h>

#include <string>
#include <unordered_map>

/* 框架提供的 专门发布rpc服务 的网络对象类 */

/* 	todo: 现在rpc客户端变成了长连接，因此rpc服务器这边最好提供一个定时器，用以断开很久没有请求的连接。
	todo：为了配合这个，那么rpc客户端那边每次发送之前也需要真正的 */ 



// 提供 RPC 服务的核心类（服务端）: 负责发布服务、接收请求、解析请求、调用本地方法、发送响应
class RpcProvider 
{
public:
	~RpcProvider();

	// 外部接口：用于注册（发布）RPC服务
	void NotifyService(google::protobuf::Service *service);

	// 启动 rpc 服务节点，开始提供 rpc 远程网络调用服务
	void Run(int nodeIndex, short port);

private:
	// Muduo 网络核心：一个事件循环 + 一个 TCP 服务器
	muduo::net::EventLoop m_eventLoop;
	std::shared_ptr<muduo::net::TcpServer> m_muduo_server;

	// service 服务类型信息 服务对象 + 服务方法
	struct ServiceInfo 
	{
		google::protobuf::Service *m_service; // 保存服务对象
		std::unordered_map<std::string, const google::protobuf::MethodDescriptor *> m_methodMap; // 保存服务方法
																						  // <方法名，方法描述>
	};

	// 存储注册成功的服务与方法  
	std::unordered_map<std::string, ServiceInfo> m_serviceMap; // <服务名，具体服务对象与其方法>
	// m_serviceMap <服务名，(服务对象 + methodMap<方法名，方法描述>)>
 

	// 三大核心回调函数：处理连接、处理 RPC 调用请求、发送响应
	
	void OnConnection(const muduo::net::TcpConnectionPtr &); // 新的socket连接回调 连接建立或断开

	void OnMessage(const muduo::net::TcpConnectionPtr &,  // 已建立连接用户的读写事件回调
				   muduo::net::Buffer *,
				   muduo::Timestamp);
	
	void SendRpcResponse(const muduo::net::TcpConnectionPtr &, // Closure的回调操作，用于序列化rpc的响应和网络发送
						 google::protobuf::Message *);
	
};