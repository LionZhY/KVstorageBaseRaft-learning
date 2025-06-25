#include "raftRpcUtil.h"

#include <mprpcchannel.h>
#include <mprpccontroller.h>


RaftRpcUtil::RaftRpcUtil(std::string ip, short port) // 接收目标节点的 IP 和端口
{
	// 发送rpc设置
	stub_ = new raftRpcProctoc::raftRpc_Stub(new MprpcChannel(ip, port, true));
	// 通过 new MprpcChannel(ip, port, true) 创建一个 RPC 通道对象
	// 再创建 raftRpc_Stub 实例，传入该通道，绑定好通信信道
	// stub_ 成为该目标节点的 RPC 客户端句柄，供后续函数调用
}


RaftRpcUtil::~RaftRpcUtil() 
{ 
	delete stub_; // 释放创建的 stub_
} 




/**
	RaftRpcUtil::AppendEntries()  RaftRpcUtil::InstallSnapshot()  RaftRpcUtil::RequestVote()
	就是封装了一下 raftRpcProctoc::raftRpc_Stub 提供的是底层的、直接的 RPC 接口

	封装之后，RaftRpcUtil::AppendEntries自动创建controller，统一失败处理逻辑，高层代码只管调用，不用接触protobuf 框架细节
	上层调用只需一行，无需关心 controller/stub/channel 创建
*/


// 追加日志 / 心跳 (封装一次对外 RPC 调用的过程)
bool RaftRpcUtil::AppendEntries(raftRpcProctoc::AppendEntriesArgs *args,		// 请求参数（由 Raft 层构造）
                                raftRpcProctoc::AppendEntriesReply *response) 	// 用于接收对方节点返回的数据
{
	MprpcController controller; // 创建 MprpcController，用于监控该 RPC 调用的状态（成功、失败、错误信息等）
	
	stub_->AppendEntries(&controller, args, response, nullptr); // 使用 stub_ 发起 RPC 调用
	// 这里调用的是 由 .proto 文件生成的远程方法调用接口，是 raftRpcProctoc::raftRpc_Stub 类中的成员函数
	
	return !controller.Failed();// 检查控制器是否失败，若未失败则返回 true
}


// 安装快照 (用于在日志滞后严重的情况下，通过快照同步状态)
bool RaftRpcUtil::InstallSnapshot(
    raftRpcProctoc::InstallSnapshotRequest *args,
    raftRpcProctoc::InstallSnapshotResponse *response) 
{
	MprpcController controller;
	stub_->InstallSnapshot(&controller, args, response, nullptr);
	return !controller.Failed();
}


// 请求投票
bool RaftRpcUtil::RequestVote(raftRpcProctoc::RequestVoteArgs *args,
                              raftRpcProctoc::RequestVoteReply *response) 
{
	MprpcController controller;
	stub_->RequestVote(&controller, args, response, nullptr);
	return !controller.Failed();
}


