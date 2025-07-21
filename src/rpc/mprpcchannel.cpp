#include "mprpcchannel.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <string>
#include "mprpccontroller.h"
#include "rpcheader.pb.h"
#include "util.h"

/*
header_size + service_name method_name args_size + args
*/


// 构造函数，保存 IP/端口，并可选择是否立即建立连接
MprpcChannel::MprpcChannel(string ip, short port, bool connectNow) : m_ip(ip), m_port(port), m_clientFd(-1) 
{
	// 使用tcp编程，完成rpc方法的远程调用，使用的是短连接，因此每次都要重新连接上去，待改成长连接。
	// 没有连接或者连接已经断开，那么就要重新连接呢,会一直不断地重试
	// 读取配置文件rpcserver的信息
	// std::string ip = MprpcApplication::GetInstance().GetConfig().Load("rpcserverip");
	// uint16_t port = atoi(MprpcApplication::GetInstance().GetConfig().Load("rpcserverport").c_str());
	// rpc调用方想调用service_name的method_name服务，需要查询zk上该服务所在的host信息
	//  /UserServiceRpc/Login
	
	if (!connectNow) {
		return;
	} 

	std::string errMsg;
	auto rt = newConnect(ip.c_str(), port, &errMsg);

	int tryCount = 3; // 如果 connectNow=true，则会尝试连接 RPC 服务端，最多重试 3 次
	while (!rt && tryCount--) 
	{
		std::cout << errMsg << std::endl;
		rt = newConnect(ip.c_str(), port, &errMsg);
	}
}





// RPC调用的主入口，stub的所有方法调用，最终都走到这个接口
// 统一通过 rpcChannel 来调用方法
// 统一做 rpc 方法调用的数据数据序列化和网络发送
void MprpcChannel::CallMethod(const google::protobuf::MethodDescriptor* method,
                              google::protobuf::RpcController* controller, 
							  const google::protobuf::Message* request,
                              google::protobuf::Message* response, 
							  google::protobuf::Closure* done) 
{
	// 检查并连接服务器，如果当前 socket 没连接，就调用 newConnect() 重连
	if (m_clientFd == -1) { 
		std::string errMsg;
		bool rt = newConnect(m_ip.c_str(), m_port, &errMsg);
		if (!rt) {
			DPrintf("[func-MprpcChannel::CallMethod]重连接ip：{%s} port{%d}失败", m_ip.c_str(), m_port);
			controller->SetFailed(errMsg);
			return;
		} else {
			DPrintf("[func-MprpcChannel::CallMethod]连接ip：{%s} port{%d}成功", m_ip.c_str(), m_port);
		}
	}

	// 准备 RPC 头部数据（自定义结构）
	const google::protobuf::ServiceDescriptor* sd = method->service();
	std::string service_name = sd->name();     // service_name 如 service_name: "FriendServiceRpc"
	std::string method_name = method->name();  // method_name  如 method_name: "GetFriendsList"

	// 序列化参数数据
	uint32_t args_size{};
	std::string args_str;
	if (request->SerializeToString(&args_str)) { // 将业务层的参数 request 转成字符串发送
		args_size = args_str.size();
	} else {
		controller->SetFailed("serialize request error!");
		return;
	}

	// 构造 RpcHeader 对象并序列化（定义见 rpcheader.proto）
	// 构建一个“消息头”，告诉对方你要调用哪个服务的哪个方法，并附带参数的长度。
	RPC::RpcHeader rpcHeader;
	rpcHeader.set_service_name(service_name);
	rpcHeader.set_method_name(method_name);
	rpcHeader.set_args_size(args_size);
	std::string rpc_header_str;
	if (!rpcHeader.SerializeToString(&rpc_header_str)) {
		controller->SetFailed("serialize rpc header error!");
		return;
	}

	// 然后用 protobuf::io::CodedOutputStream 构造最终发送内容
	// [header_size(varint)] [rpcHeader serialized] [args serialized]
	std::string send_rpc_str;  // 存储最终发送的数据
	{
		// 创建一个StringOutputStream用于写入send_rpc_str
		google::protobuf::io::StringOutputStream string_output(&send_rpc_str);
		google::protobuf::io::CodedOutputStream coded_output(&string_output);

		coded_output.WriteVarint32(static_cast<uint32_t>(rpc_header_str.size()));// 先写入header的长度（变长编码）
		coded_output.WriteString(rpc_header_str);// 然后写入rpc_header内容
		// 不需要手动写入header_size，因为上面的WriteVarint32已经包含了header的长度信息
	}

	// 最后，将请求参数附加到send_rpc_str后面
	send_rpc_str += args_str;

	// 打印调试信息
	//    std::cout << "============================================" << std::endl;
	//    std::cout << "header_size: " << header_size << std::endl;
	//    std::cout << "rpc_header_str: " << rpc_header_str << std::endl;
	//    std::cout << "service_name: " << service_name << std::endl;
	//    std::cout << "method_name: " << method_name << std::endl;
	//    std::cout << "args_str: " << args_str << std::endl;
	//    std::cout << "============================================" << std::endl;

	// 发送rpc请求 send()，失败会重试连接再发送，重试连接失败会直接return
	while (-1 == send(m_clientFd, send_rpc_str.c_str(), send_rpc_str.size(), 0)) 
	{
		char errtxt[512] = {0};
		sprintf(errtxt, "send error! errno:%d", errno);
		std::cout << "尝试重新连接，对方ip：" << m_ip << " 对方端口" << m_port << std::endl;
		close(m_clientFd);

		m_clientFd = -1;
		std::string errMsg;
		bool rt = newConnect(m_ip.c_str(), m_port, &errMsg);
		if (!rt) {
			controller->SetFailed(errMsg);
			return;
		}
	}

	/*
	从时间节点来说，这里将请求发送过去之后rpc服务的提供者就会开始处理，返回的时候就代表着已经返回响应了
	*/

	// 接收rpc请求的响应 recv()
	char recv_buf[1024] = {0};
	int recv_size = 0;
	if (-1 == (recv_size = recv(m_clientFd, recv_buf, 1024, 0))) 
	{
		close(m_clientFd);
		m_clientFd = -1;
		char errtxt[512] = {0};
		sprintf(errtxt, "recv error! errno:%d", errno);
		controller->SetFailed(errtxt);
		return;
	}

	// 使用 ParseFromArray 避免 \0 截断问题

	if (!response->ParseFromArray(recv_buf, recv_size))  // 读取服务器响应，填充到 response
	{
		char errtxt[1050] = {0};
		sprintf(errtxt, "parse error! response_str:%s", recv_buf);
		controller->SetFailed(errtxt);
		return;
	}

	// 所有步骤（序列化失败、发送失败、接收失败、反序列化失败）都会设置 controller->SetFailed(...)，用于上层判断失败原因。
}




// 建立连接（或重连），设置 m_clientFd
bool MprpcChannel::newConnect(const char* ip, uint16_t port, string* errMsg) 
{
	int clientfd = socket(AF_INET, SOCK_STREAM, 0);
	if (-1 == clientfd) {
		char errtxt[512] = {0};
		sprintf(errtxt, "create socket error! errno:%d", errno);
		m_clientFd = -1;
		*errMsg = errtxt;
		return false;
	}

	struct sockaddr_in server_addr;
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port);
	server_addr.sin_addr.s_addr = inet_addr(ip);

	// 连接rpc服务节点
	if (-1 == connect(clientfd, (struct sockaddr*)&server_addr, sizeof(server_addr))) {
		close(clientfd);
		char errtxt[512] = {0};
		sprintf(errtxt, "connect fail! errno:%d", errno);
		m_clientFd = -1;
		*errMsg = errtxt;
		return false;
	}

	m_clientFd = clientfd;
	return true;
}