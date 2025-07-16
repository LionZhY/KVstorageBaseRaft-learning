#ifndef RAFTSERVERRPC_H
#define RAFTSERVERRPC_H

#include <iostream>
#include "kvServerRPC.pb.h"
#include "mprpcchannel.h"
#include "mprpccontroller.h"
#include "rpcprovider.h"



/* raftServerRPCUtil 是封装 【Clerk 客户端 与 Raft 集群中每个 KVServer 节点】RPC通信的封装类 */

class raftServerRpcUtil 
{
public:
    raftServerRpcUtil(std::string ip, short port);
    ~raftServerRpcUtil();

    //主动调用其他节点的三个方法,可以按照mit6824来调用，但是别的节点调用自己的好像就不行了，要继承protoc提供的service类才行

    // 发起 RPC 调用，向某个节点的 kvServer 发出请求
    bool Get(raftKVRpcProctoc::GetArgs* GetArgs, raftKVRpcProctoc::GetReply* reply);
    bool PutAppend(raftKVRpcProctoc::PutAppendArgs* args, raftKVRpcProctoc::PutAppendReply* reply);

    
private:
    raftKVRpcProctoc::kvServerRpc_Stub* stub; // RPC 客户端接口（Stub） (gRPC风格的远程调用Stub, 由proto编译器生成)

};



#endif  // RAFTSERVERRPC_H