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
#include "ApplyMsg.h"       // 日志条目或快照应用到状态机时的消息格式
#include "kvServerRPC.pb.h" // KVServer RPC协议定义（protobuf自动生成）
#include "raft.h"           // Raft一致性算法相关实现
#include "skipList.h"       // 跳表数据结构实现
#include "util.h"


class KvServer : raftKVRpcProctoc::kvServerRpc 
{				// 继承自 raftKVRpcProctoc 命名空间中的 kvServerRpc 类 (Protobuf 自动生成的接口类 kvServerRpc)
				// 表示 KVServer 是一个可通过 RPC 被 Clerk（客户端）调用的服务端节点
public:

    KvServer() = delete; // 禁止默认构造
    KvServer(int me, int maxraftstate, std::string nodeInforFileName, short port);

	void DprintfKVDB(); // 打印当前 kv 数据，调试用


	///////////////////////////////////////////  KVServer 启动入口  ///////////////////////////////////////////
	
	void StartKVServer(); // 启动 RPC 服务，并初始化 raft 节点 （没实现）



	///////////////////////////////////////////  客户端请求执行  ///////////////////////////////////////////

    void ExecuteAppendOpOnKVDB(Op op);								// 执行 append
    void ExecuteGetOpOnKVDB(Op op, std::string* value, bool* exist);// 执行 get，并返回是否存在
    void ExecutePutOpOnKVDB(Op op);									// 执行 put

    bool ifRequestDuplicate(std::string ClientId, int RequestId);   // 判断请求是否是重复提交，用于实现幂等性



	///////////////////////////////////////////   与 raft 交互   ///////////////////////////////////////////

    void GetCommandFromRaft(ApplyMsg message); 				 // Raft commit 某条日志后回调，用于驱动状态机执行
	bool SendMessageToWaitChan(const Op &op, int raftIndex); // 提交结果写回等待通道
    void ReadRaftApplyCommandLoop(); 						 // 持续从 applyChan 获取 Raft 提交的日志项



	/////////////////////////////////////////  Clerk 触发的远程请求  /////////////////////////////////////////
	
	// clerk 使用RPC远程调用 （	调用 Raft::Start 提交操作；收到 commit 后响应 Clerk ）
	void PutAppend(const raftKVRpcProctoc::PutAppendArgs *args, 
				   raftKVRpcProctoc::PutAppendReply *reply);
    void Get(const raftKVRpcProctoc::GetArgs* args,
             raftKVRpcProctoc::GetReply* reply); //将 GetArgs 改为rpc调用的，因为是远程客户端，即服务器宕机对客户端来说是无感的
    


	////////////////////////////////////////////  快照机制支持  ////////////////////////////////////////////

    void IfNeedToSendSnapShotCommand(int raftIndex, int proportion); // 判断是否需要触发快照，交由 Raft 层处理
    std::string MakeSnapShot(); 					  // 调用序列化函数生成快照
	void ReadSnapShotToInstall(std::string snapshot); // 从快照恢复本地状态机
    void GetSnapShotFromRaft(ApplyMsg message); 	  // Raft 层传来快照，判断是否安装并恢复



public:  
	//////////////////////////////////////////  RPC 对外接口重写  //////////////////////////////////////////

	// 对外接口（对clerk）  PutAppend / Get（重载版本，供 Protobuf RPC 调用）
    void PutAppend(google::protobuf::RpcController *controller, 
                   const ::raftKVRpcProctoc::PutAppendArgs *request,
                   ::raftKVRpcProctoc::PutAppendReply *response, 
                   ::google::protobuf::Closure *done) override;

    void Get(google::protobuf::RpcController *controller, 
             const ::raftKVRpcProctoc::GetArgs *request,
             ::raftKVRpcProctoc::GetReply *response, 
             ::google::protobuf::Closure *done) override;



private:
	///////////////////////////////////////////////  序列化  ///////////////////////////////////////////////

    friend class boost::serialization::access;

    // When the class Archive corresponds to an output archive, the & operator is defined similar to <<.  
	// Likewise, when the class Archive is a type of input archive, the & operator is defined similar to >>.
    
	// 只序列化了两个字段：m_serializedKVData  m_lastRequestId
	template <class Archive>
    void serialize(Archive &ar, const unsigned int version)
    {
    	ar &m_serializedKVData; // 存储跳表序列化结果，用于快照
    	ar &m_lastRequestId;	// 用于保证幂等性：记录每个客户端最后一次请求 ID
    }

	// 从跳表中获取序列化内容 → 序列化整个对象为字符串
    std::string getSnapshotData() 
    {
    	m_serializedKVData = m_skipList.dump_file();
    	std::stringstream ss;
    	boost::archive::text_oarchive oa(ss);
    	oa << *this;
    	m_serializedKVData.clear();
    	return ss.str();
    }

	// 从字符串中恢复出原对象，恢复跳表状态
    void parseFromString(const std::string &str) 
    {
    	std::stringstream ss(str);
    	boost::archive::text_iarchive ia(ss);
    	ia >> *this;
    	m_skipList.load_file(m_serializedKVData);
    	m_serializedKVData.clear();
    }



private:

    std::mutex m_mtx;

    int m_me; // 当前服务器的id
	
    std::shared_ptr<Raft> m_raftNode; // 当前节点绑定的 Raft 实例
    std::shared_ptr<LockQueue<ApplyMsg>> applyChan; // kvServer和raft节点的通信管道

    int m_maxRaftState; 			// 日志大小上限，超出时触发快照
	int m_lastSnapShotRaftLogIndex; // 最近一次快照的日志索引

    std::string m_serializedKVData; // 存储跳表序列化结果，用于快照 (理论上可以不用)

    SkipList<std::string, std::string> m_skipList; 		 // 实际存储的本地状态机
    std::unordered_map<std::string, std::string> m_kvDB; // 备用的哈希表实现，不使用

    std::unordered_map<int, LockQueue<Op>*> waitApplyCh; // 通过日志索引等待 Raft commit 的 Clerk 通道

    std::unordered_map<std::string, int> m_lastRequestId;// 用于保证幂等性：记录每个客户端最后一次请求 ID
    

    
};




#endif