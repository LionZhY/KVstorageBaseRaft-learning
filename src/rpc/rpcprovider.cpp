#include "rpcprovider.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <fstream>
#include <string>
#include "rpcheader.pb.h"
#include "util.h"
/*
service_name =>  service描述
                        =》 service* 记录服务对象
                        method_name  =>  method方法对象
json   protobuf
*/

// 这里是框架提供给外部使用的，可以发布rpc方法的函数接口
// 只是简单的把服务描述符和方法描述符全部保存在本地而已
// todo 待修改 要把本机开启的ip和端口写在文件里面


RpcProvider::~RpcProvider() 
{
	std::cout << "[func - RpcProvider::~RpcProvider()]: ip和port信息：" << m_muduo_server->ipPort() << std::endl;
	m_eventLoop.quit();
	//    m_muduo_server.   怎么没有stop函数，奇奇怪怪，看csdn上面的教程也没有要停止，甚至上面那个都没有
}



// 外部接口：用于注册（发布）RPC服务
void RpcProvider::NotifyService(google::protobuf::Service *service) 
{
	/* 注册一个 protobuf::Service 类型的服务（服务类通常继承自 xx::Service，例如 UserService::Service） */
	
	ServiceInfo service_info; // 服务对象 + 服务方法

	const google::protobuf::ServiceDescriptor *pserviceDesc = service->GetDescriptor(); // 获取服务描述符
	std::string service_name = pserviceDesc->name(); // 获取服务的名字
	

	// 遍历服务中的所有方法，提取方法名及其描述信息，记录到 m_methodMap
	int methodCnt = pserviceDesc->method_count(); // 获取service的方法的数量
	std::cout << "service_name:" << service_name << std::endl;
	for (int i = 0; i < methodCnt; ++i) 
	{
		const google::protobuf::MethodDescriptor *pmethodDesc = pserviceDesc->method(i); // 获取方法描述符
		std::string method_name = pmethodDesc->name();
		service_info.m_methodMap.insert({method_name, pmethodDesc}); // <方法名，方法描述>
	}

	// 保存服务指针及方法信息，存到 m_serviceMap
	service_info.m_service = service;
	m_serviceMap.insert({service_name, service_info}); // <服务名，serviceinfo>
}



// 启动 rpc 服务节点，开始提供rpc远程网络调用服务
void RpcProvider::Run(int nodeIndex, short port) 
{
	/* 负责启动一个 RPC 服务节点，让它开始监听客户端发来的 RPC 请求，核心是基于 Muduo 网络库 的 TCP 服务器封装 */

	// 获取本机的一个可用 IP 地址，用于绑定 RPC 服务的监听地址
	char *ipC;
	char hname[128];
	struct hostent *hent;
	gethostname(hname, sizeof(hname));			// 获取本机主机名
	hent = gethostbyname(hname);				// 获取主机信息（含IP）
	for (int i = 0; hent->h_addr_list[i]; i++) 
	{
		ipC = inet_ntoa(*(struct in_addr *)(hent->h_addr_list[i]));  // 转换为点分十进制 IP
	}
	std::string ip = std::string(ipC); // 转为 C++ 字符串

	//    // 获取端口
	//    if(getReleasePort(port)) //在port的基础上获取一个可用的port，不知道为何没有效果
	//    {
	//        std::cout << "可用的端口号为：" << port << std::endl;
	//    }
	//    else
	//    {
	//        std::cout << "获取可用端口号失败！" << std::endl;
	//    }


	// 将服务节点的 ip 和 port 信息写入配置文件 test.conf，方便客户端或注册中心读取
	std::string node = "node" + std::to_string(nodeIndex); // 形如 node1、node2
	std::ofstream outfile;
	outfile.open("test.conf", std::ios::app);  //打开文件并追加写入
	if (!outfile.is_open()) 
	{
		std::cout << "打开文件失败！" << std::endl;
		exit(EXIT_FAILURE);
	}
	outfile << node + "ip=" + ip << std::endl;
	outfile << node + "port=" + std::to_string(port) << std::endl;
	outfile.close();


	// 基于 Muduo 创建 TCP 服务，监听传入的 IP/端口 （绑定地址、事件循环、服务名）
	muduo::net::InetAddress address(ip, port);
	m_muduo_server = std::make_shared<muduo::net::TcpServer>(&m_eventLoop, address, "RpcProvider");


	// 注册连接事件回调（连接建立 / 关闭）  和消息事件回调（有数据到达时触发）
	m_muduo_server->setConnectionCallback(std::bind(&RpcProvider::OnConnection, this, std::placeholders::_1));
	m_muduo_server->setMessageCallback(
		std::bind(&RpcProvider::OnMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));


	// 设置muduo库的线程数量
	m_muduo_server->setThreadNum(4);

	std::cout << "RpcProvider start service at ip:" << ip << " port:" << port << std::endl;

	// 启动网络服务 进入事件循环等待连接
	m_muduo_server->start(); 
	m_eventLoop.loop();

}




// 新的socket连接回调 连接建立或断开
void RpcProvider::OnConnection(const muduo::net::TcpConnectionPtr &conn) 
{
	// 如果是新连接就什么都不干，即正常的接收连接即可

	if (!conn->connected()) 
	{ 
		conn->shutdown(); // 如果客户端断开连接，则关闭 socket
	}
}



/*
在框架内部，RpcProvider和RpcConsumer协商好之间通信用的protobuf数据类型
service_name method_name args    定义proto的message类型，进行数据头的序列化和反序列化
                                 service_name method_name args_size
16UserServiceLoginzhang san123456

header_size(4个字节) + header_str + args_str
10 "10"
10000 "1000000"
std::string   insert和copy方法
*/



// 已建立连接用户的读写事件回调
void RpcProvider::OnMessage(const muduo::net::TcpConnectionPtr &conn, 
							muduo::net::Buffer *buffer, 
							muduo::Timestamp) 
{
	/*  客户端发送数据时触发，接收请求 → 查找服务方法 → 执行方法 → 回包 */

	std::string recv_buf = buffer->retrieveAllAsString(); // 网络上接收的远程rpc调用请求的字符流（包含：header_size + header + args）

	// 使用protobuf的CodedInputStream来提取并反序列化 Header 信息
	// 创建 protobuf 输入流解析器
	google::protobuf::io::ArrayInputStream array_input(recv_buf.data(), recv_buf.size());
	google::protobuf::io::CodedInputStream coded_input(&array_input);
	

	uint32_t header_size{};
	coded_input.ReadVarint32(&header_size);  // 提取 header 的大小（变长编码）

	// 根据 header_size 读取数据头的原始字符流，Header 包含客户端想调用的服务名、方法名、参数字节长度
	std::string rpc_header_str;
	RPC::RpcHeader rpcHeader; // RPC 自定义头部
	std::string service_name;
	std::string method_name;

	// 设置读取限制，不必担心数据读多
	google::protobuf::io::CodedInputStream::Limit msg_limit = coded_input.PushLimit(header_size);
	coded_input.ReadString(&rpc_header_str, header_size); // 读取 header 的实际内容
	coded_input.PopLimit(msg_limit); // 恢复之前的限制，以便安全地继续读取其他数据
	
	// 从 header 中提取 service、method、args_size 信息
	uint32_t args_size{};
	if (rpcHeader.ParseFromString(rpc_header_str))  // 数据头反序列化成功
	{
		service_name = rpcHeader.service_name();
		method_name = rpcHeader.method_name();
		args_size = rpcHeader.args_size();
	} 
	else  // 数据头反序列化失败
	{
		std::cout << "rpc_header_str:" << rpc_header_str << " parse error!" << std::endl;
		return;
	}

	
	// 读取实际参数 args
	std::string args_str; // 获取rpc方法参数的字符流数据
	bool read_args_success = coded_input.ReadString(&args_str, args_size); // 直接读取args_size长度的字符串数据
	if (!read_args_success)	return;// 处理错误：参数数据读取失败
		

	// 打印调试信息
	//    std::cout << "============================================" << std::endl;
	//    std::cout << "header_size: " << header_size << std::endl;
	//    std::cout << "rpc_header_str: " << rpc_header_str << std::endl;
	//    std::cout << "service_name: " << service_name << std::endl;
	//    std::cout << "method_name: " << method_name << std::endl;
	//    std::cout << "args_str: " << args_str << std::endl;
	//    std::cout << "============================================" << std::endl;



	// 查找注册的服务（必须用户提前注册）
	auto it = m_serviceMap.find(service_name);
	if (it == m_serviceMap.end()) // 服务不存在
	{
		std::cout << "服务：" << service_name << " is not exist!" << std::endl;
		std::cout << "当前已经有的服务列表为:";
		for (auto item : m_serviceMap) {
			std::cout << item.first << " ";
		}
		std::cout << std::endl;
		return;
	}
	// 查找该服务下的方法
	auto mit = it->second.m_methodMap.find(method_name);
	if (mit == it->second.m_methodMap.end()) {
		std::cout << service_name << ":" << method_name << " is not exist!" << std::endl;
		return;
	}

	google::protobuf::Service *service = it->second.m_service;       // 获取service对象  new UserService
	const google::protobuf::MethodDescriptor *method = mit->second;  // 获取method对象  Login


	// 创建rpc方法调用的请求 request 和响应 response 参数, 由于是rpc的请求，因此请求需要通过request来序列化
	google::protobuf::Message *request = service->GetRequestPrototype(method).New();
	if (!request->ParseFromString(args_str)) 
	{
		std::cout << "request parse error, content:" << args_str << std::endl;
		return;
	}
	google::protobuf::Message *response = service->GetResponsePrototype(method).New();


	// 给下面的method方法的调用，绑定一个Closure的回调函数
	// closure是执行完本地方法之后会发生的回调，因此需要完成序列化和反向发送请求的操作

	// 构造回调（用于响应 RPC 客户端）
	google::protobuf::Closure *done =
		google::protobuf::NewCallback<RpcProvider, const muduo::net::TcpConnectionPtr &, google::protobuf::Message *>(
			this, &RpcProvider::SendRpcResponse, conn, response);


	// 在框架上根据远端rpc请求，调用当前rpc节点上发布的方法
	// new UserService().Login(controller, request, response, done)

	/*
	为什么下面这个service->CallMethod 要这么写？或者说为什么这么写就可以直接调用远程业务方法了
	这个service在运行的时候会是注册的service
	// 用户注册的service类 继承 .protoc生成的serviceRpc类 继承 google::protobuf::Service
	// 用户注册的service类里面没有重写CallMethod方法，是 .protoc生成的serviceRpc类 里面重写了google::protobuf::Service中
	的纯虚函数CallMethod，而 .protoc生成的serviceRpc类 会根据传入参数自动调取 生成的xx方法（如Login方法），
	由于xx方法被 用户注册的service类 重写了，因此这个方法运行的时候会调用 用户注册的service类 的xx方法
	真的是妙呀
	*/


	// 真正调用远程方法（调用用户自定义的 Login/Register 方法）
	service->CallMethod(method, nullptr, request, response, done);
}



// Closure 的回调操作，将服务端执行完方法后的响应结果返回给客户端
void RpcProvider::SendRpcResponse(const muduo::net::TcpConnectionPtr &conn, google::protobuf::Message *response) 
{
	/* 把方法调用后的响应数据序列化后，通过网络发送回调用方客户 */
	
	std::string response_str;
	if (response->SerializeToString(&response_str))  // response进行序列化
	{
		conn->send(response_str); // 序列化成功后，将响应内容写入 TCP 连接，由 Muduo 框架负责实际网络发送
	} 
	else 
	{
		std::cout << "serialize response_str error!" << std::endl;
	}
	//    conn->shutdown(); // 模拟http的短链接服务，由rpcprovider主动断开连接  //改为长连接，不主动断开
}

