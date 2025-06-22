#ifndef SKIP_LIST_ON_RAFT_KVSERVER_H
#define SKIP_LIST_ON_RAFT_KVSERVER_H

// Boost库相关头文件
#include <boost/any.hpp>                         // 提供类型安全的任意类型容器
#include <boost/archive/binary_iarchive.hpp>     // 用于二进制反序列化
#include <boost/archive/binary_oarchive.hpp>     // 用于二进制序列化
#include <boost/archive/text_iarchive.hpp>       // 用于文本反序列化
#include <boost/archive/text_oarchive.hpp>       // 用于文本序列化
#include <boost/foreach.hpp>                     // 提供BOOST_FOREACH宏，便于遍历容器
#include <boost/serialization/export.hpp>        // 支持多态类型的序列化导出
#include <boost/serialization/serialization.hpp> // 提供序列化的基础设施
#include <boost/serialization/unordered_map.hpp> // 支持unordered_map的序列化
#include <boost/serialization/vector.hpp>        // 支持vector的序列化

// 标准库头文件
#include <iostream>         // 标准输入输出流
#include <mutex>            // 互斥锁，用于多线程同步
#include <unordered_map>    // 哈希表容器

// 项目内头文件
#include "kvServerRPC.pb.h" // KVServer RPC协议定义（protobuf自动生成）
#include "raft.h"           // Raft一致性算法相关实现
#include "skipList.h"       // 跳表数据结构实现


class KvServer : raftKVRpcProctoc::kvServerRpc // 继承自 raftKVRpcProctoc 命名空间中的 kvServerRpc 类 (Protobuf 自动生成的接口类 kvServerRpc)
{
private:

    std::mutex m_mtx;
    int m_me;
    std::shared_ptr<Raft> m_raftNode;
    std::shared_ptr<LockQueue<ApplyMsg>> applyChan; // kvServer和raft节点的通信管道
    int m_maxRaftState; // snapshot if log grows this big


    // your definitions here
    std::string m_seralizedKVData; // // todo ： 序列化后的kv数据，理论上可以不用，但是目前没有找到特别好的替代方法
    








public:









    
};






#endif