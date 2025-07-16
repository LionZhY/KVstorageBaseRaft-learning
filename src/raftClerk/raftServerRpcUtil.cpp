#include "raftServerRpcUtil.h"

// kvserver不同于raft节点之间，kvserver的rpc是用于clerk向kvserver调用，不会被调用，因此只用写caller功能，不用写callee功能
// 先开启服务器，再尝试连接其他的节点，中间给一个间隔时间，等待其他的rpc服务器节点启动

raftServerRpcUtil::raftServerRpcUtil(std::string ip, short port) 
{
	stub = new raftKVRpcProctoc::kvServerRpc_Stub( new MprpcChannel(ip, port, false) );
	// MprpcChannel(ip, port, false) 创建一个用于RPC调用的通信通道
	// 然后使用该通道创建stub
}


raftServerRpcUtil::~raftServerRpcUtil() 
{ 
	delete stub; // 手动释放 stub 占用的堆内存
}



//  RPC 调用封装函数
bool raftServerRpcUtil::Get(raftKVRpcProctoc::GetArgs *GetArgs, raftKVRpcProctoc::GetReply *reply) 
{
	MprpcController controller; 					 // 控制此次 RPC 请求状态（失败/错误码等）
	stub->Get(&controller, GetArgs, reply, nullptr); // 实际发起 RPC 调用，底层通过 MprpcChannel 发送请求
	return !controller.Failed();
}

bool raftServerRpcUtil::PutAppend(raftKVRpcProctoc::PutAppendArgs *args, 
								  raftKVRpcProctoc::PutAppendReply *reply) 
{
	MprpcController controller;
	stub->PutAppend(&controller, args, reply, nullptr); // 调用远端的 PutAppend() 方法
	if (controller.Failed()) 
	{
	  std::cout << controller.ErrorText() << endl;
	}

	return !controller.Failed();
}