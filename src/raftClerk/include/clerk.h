#ifndef SKIP_LIST_ON_RAFT_CLERK_H
#define SKIP_LIST_ON_RAFT_CLERK_H
#include <arpa/inet.h>
#include <netinet/in.h>
#include <raftServerRpcUtil.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <string>
#include <vector>
#include "kvServerRPC.pb.h"
#include "mprpcconfig.h"


/* clerk相当于就是一个外部的客户端，其作用就是向整个raft集群发起命令并接收响应。 */


class Clerk 
{
public:
    Clerk();
    
    // 初始化连接
    void Init(std::string configFileName);

    // 对外暴露的三个功能
    std::string Get(std::string key);
    void Put(std::string key, std::string value);
    void Append(std::string key, std::string value);

    
private:
    std::vector<std::shared_ptr<raftServerRpcUtil>> m_servers; // 保存所有 Raft 节点 RPC 通信对象 (raftServerRpcUtil)
    std::string m_clientId; // 随机生成的客户端id（用于幂等控制）
    int m_requestId;		// 每次请求自增（区别不同请求）
    int m_recentLeaderId;  	// 记录 “上一次成功请求” 的节点 id，作为当前可能的 Leader

    // 随机生成ClientId
    std::string Uuid() 
    {
        return std::to_string(rand()) + std::to_string(rand()) + std::to_string(rand()) + std::to_string(rand());
    }  

    // 统一处理Put/Append
    void PutAppend(std::string key, std::string value, std::string op);
};


#endif  // SKIP_LIST_ON_RAFT_CLERK_H