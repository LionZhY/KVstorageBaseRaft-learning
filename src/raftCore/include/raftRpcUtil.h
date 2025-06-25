#ifndef RAFTRPC_H
#define RAFTRPC_H

#include "raftRPC.pb.h"

// RaftRpcUtil: 用来封装 Raft 节点之间 RPC 通信的客户端功能，即每个节点用于向其他节点发起 RPC 请求的工具类
// 它不是用来处理接收请求的，而是专门用来主动发送 RPC 调用的

// 对于一个raft节点来说，对于任意其他的节点都要维护一个rpc连接，即MprpcChannel

// 用于 Raft 节点之间的远程通信
// 每个 Raft 节点在启动时会为其他每个节点创建一个 RaftRpcUtil 实例，用来向其它对应节点发起网络调用（如心跳、日志同步、投票等）。


class RaftRpcUtil {
private:
	raftRpcProctoc::raftRpc_Stub *stub_; // 由 proto 文件生成的客户端代理类（Stub），用于发送 RPC 请求
										 // 其本质是封装了与目标节点通信的信道 MprpcChannel，相当于“客户端的远程方法调用接口”

public:
	RaftRpcUtil(std::string ip, short port);
	~RaftRpcUtil();

	// 通过 RaftRpcUtil，当前节点可以向目标节点发起以下三种 RPC 调用：
	bool AppendEntries(raftRpcProctoc::AppendEntriesArgs *args, raftRpcProctoc::AppendEntriesReply *response);
	bool InstallSnapshot(raftRpcProctoc::InstallSnapshotRequest *args, raftRpcProctoc::InstallSnapshotResponse *response);
	bool RequestVote(raftRpcProctoc::RequestVoteArgs *args, raftRpcProctoc::RequestVoteReply *response);

};

#endif  // RAFTRPC_H