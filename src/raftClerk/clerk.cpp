#include <string>
#include <vector>

#include "clerk.h"
#include "raftServerRpcUtil.h" // 用于与 Raft 节点通信
#include "util.h"




Clerk::Clerk() : m_clientId(Uuid()), m_requestId(0), m_recentLeaderId(0) {} // 初始假设第 0 号节点是 leader



// 初始化连接
void Clerk::Init(std::string configFileName) 
{
    // 加载配置文件，获取所有raft节点ip、port ，并进行连接
    MprpcConfig config;
    config.LoadConfigFile(configFileName.c_str()); // 使用 MPRPC 框架配置模块读取 .conf 文件，获取集群信息

    // 保存每个节点的 ip - port 到 ipPortVt
    std::vector<std::pair<std::string, short>> ipPortVt; 
    for (int i = 0; i < INT_MAX - 1; ++i) 
    {
        std::string node = "node" + std::to_string(i);

        std::string nodeIp = config.Load(node + "ip");
        std::string nodePortStr = config.Load(node + "port");
        if (nodeIp.empty()) 
        {
            break;
        }

        ipPortVt.emplace_back(nodeIp, atoi(nodePortStr.c_str())); 
    }

    // 建立与每个节点的 RPC 连接
    for (const auto& item : ipPortVt) 
    {
        std::string ip = item.first;
        short port = item.second;

        // 每个地址创建一个 raftServerRpcUtil 对象，收集到 m_servers 中
        auto* rpc = new raftServerRpcUtil(ip, port); 
        m_servers.push_back(std::shared_ptr<raftServerRpcUtil>(rpc)); // 用智能指针包裹，统一管理资源
    }

}




std::string Clerk::Get(std::string key) 
{
    m_requestId++; // 每次请求自增 requestId
    auto requestId = m_requestId;
    
    int server = m_recentLeaderId; // 默认先访问最近成功访问过的 Leader

    // 构造 GetArgs 请求结构（key + 唯一请求标识）
    raftKVRpcProctoc::GetArgs args;
    args.set_key(key);
    args.set_clientid(m_clientId);
    args.set_requestid(requestId);

    // 尝试发送请求并循环直到成功
    while (true) 
    {
        raftKVRpcProctoc::GetReply reply;
        bool ok = m_servers[server]->Get(&args, &reply); // 向当前 server 节点发起 RPC 调用
        
        if (!ok || reply.err() == ErrWrongLeader) // 通信失败 或目标节点不是 leader，尝试下一个节点
        {  
            server = (server + 1) % m_servers.size();
            continue;
        }

        if (reply.err() == ErrNoKey) // 通信ok，但 key 不存在，返回空字符串
        {
            return "";
        }
        if (reply.err() == OK) // 通信ok，且读取成功，更新当前 Leader ID，返回value
        {
            m_recentLeaderId = server;
            return reply.value();
        }
    }

    return "";
}



// 统一处理 Put/Append
void Clerk::PutAppend(std::string key, std::string value, std::string op) 
{

    m_requestId++; // 每次请求自增 requestId
    auto requestId = m_requestId;

    auto server = m_recentLeaderId; // 默认先访问最近成功访问过的 Leader

    while (true) 
    {
        // 构造请求结构体，包含所有字段
        raftKVRpcProctoc::PutAppendArgs args;
        args.set_key(key);
        args.set_value(value);
        args.set_op(op); // op 表示 "Put" 或 "Append"
        args.set_clientid(m_clientId);
        args.set_requestid(requestId);

        raftKVRpcProctoc::PutAppendReply reply;


        // 发起 RPC 请求
        bool ok = m_servers[server]->PutAppend(&args, &reply); // 发送到当前 server节点

        if (!ok || reply.err() == ErrWrongLeader) // 通信失败，或者目标节点不是leader，尝试下一个节点
        {
            DPrintf("【Clerk::PutAppend】原以为的leader：{%d}请求失败，向新leader{%d}重试  ，操作：{%s}", 
                      server, server + 1, op.c_str());
            
            if (!ok)                              DPrintf("重试原因 ，rpc失敗 ，");
            if (reply.err() == ErrWrongLeader)    DPrintf("重试原因：非leader");

            server = (server + 1) % m_servers.size();  // 尝试下一个节点
            continue;
        }

        if (reply.err() == OK) // 通信ok，且请求成功，直接返回
        {  
            m_recentLeaderId = server;
            return;
        }
    }
}



// 单独 Put 接口
void Clerk::Put(std::string key, std::string value) 
{ 
    PutAppend(key, value, "Put"); 
}


// 单独 Append 接口
void Clerk::Append(std::string key, std::string value) 
{ 
    PutAppend(key, value, "Append"); 
}




