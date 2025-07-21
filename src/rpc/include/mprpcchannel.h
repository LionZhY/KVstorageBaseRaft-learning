#ifndef MPRPCCHANNEL_H
#define MPRPCCHANNEL_H

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>
#include <algorithm>
#include <algorithm>  // 包含 std::generate_n() 和 std::generate() 函数的头文件
#include <functional>
#include <iostream>
#include <map>
#include <random>  // 包含 std::uniform_int_distribution 类型的头文件
#include <string>
#include <unordered_map>
#include <vector>
using namespace std;


// MprpcChannel : RPC 框架中的客户端通信通道，真正负责发送和接受的前后处理工作
// 将客户端 stub 发起的调用请求序列化、发送至服务器、接收响应并反序列化结果。

class MprpcChannel : public google::protobuf::RpcChannel 
{
public:
	// 构造函数，保存 IP/端口，并可选择是否立即建立连接
	MprpcChannel(string ip, short port, bool connectNow);

	// RPC调用的主入口，stub的所有方法调用，最终都走到这个接口
	void CallMethod(const google::protobuf::MethodDescriptor *method, 
					google::protobuf::RpcController *controller,
					const google::protobuf::Message *request, 
					google::protobuf::Message *response,
					google::protobuf::Closure *done) override;


private:
	int m_clientFd;			// socket 文件描述符
	const std::string m_ip; // RPC服务端IP
	const uint16_t m_port;	// RPC服务端端口

	
	// 建立连接（或重连），连接指定的 IP 和端口，返回连接是否成功
	bool newConnect(const char *ip, uint16_t port, string *errMsg);

	/// @brief 连接ip和端口, 并设置m_clientFd
	/// @param ip ip地址，本机字节序
	/// @param port 端口，本机字节序
	/// @return 成功返回空字符串，否则返回失败信息
};


#endif  // MPRPCCHANNEL_H