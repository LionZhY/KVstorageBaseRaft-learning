#ifndef RAFT_H
#define RAFT_H

#include <boost/serialization/string.hpp>
#include <boost/serialization/vector.hpp>

#include <chrono>   // 提供时间操作
#include <cmath>    
#include <iostream> 
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ApplyMsg.h"    // 应用层消息结构体，用于提交日志到状态机
#include "Persister.h"   // 封装持久化接口，保存日志和状态
#include "boost/any.hpp" // 通用类型封装，在 Op 命令中使用
#include "boost/serialization/serialization.hpp"

#include "iomanager.hpp"
#include "monsoon.h"     // 协程管理，支持异步任务调度
#include "raftRPC.pb.h"
#include "raftRpcUtil.h" // RPC调用封装，方便远程节点通信
#include "config.h"      // 一些全局参数
#include "util.h"        // 工具函数



// 网络和投票状态
constexpr int Disconnected = 0; // 标识网络断开或分区，方便调试网络异常导致的状态
constexpr int AppNormal = 1;    // 标识网络连接正常


// 投票状态
constexpr int Killed = 0; // 投票线程被杀死或终止
constexpr int Voted = 1;  // 当前任期内已经投过票
constexpr int Expire = 2; // 当前投票请求（消息，竞选者）过期或超时
constexpr int Normal = 3; // 投票状态正常


/**
	Raft 是实现 Raft 协议核心逻辑的类
*/


class Raft : public raftRpcProctoc::raftRpc // 继承 Protobuf 生成的 RPC 基类 raftRpc，实现远程过程调用接口，供集群内节点之间通信
{
public:
	// 初始化 ---------------------------------------------------------------------------------------
	// 初始化Raft节点，传入集群成员代理、节点ID、持久化模块和日志应用通道
	void init(std::vector<std::shared_ptr<RaftRpcUtil>> peers, int me,
			  std::shared_ptr<Persister> persister,
			  std::shared_ptr<LockQueue<ApplyMsg>> applyCh);
	

	
	// 选举 ------------------------------------------------------------------------------------------
	void electionTimeOutTicker(); // 选举定时器：周期性检查选举是否超时，触发新一轮选举
	void doElection(); 			  // 发起一次选举
	
	// candidate发送请求投票RPC给指定服务器
	bool sendRequestVote(int server,
				 	 	 std::shared_ptr<raftRpcProctoc::RequestVoteArgs> args,
				 	 	 std::shared_ptr<raftRpcProctoc::RequestVoteReply> reply,
				 	 	 std::shared_ptr<int> votedNum);
	
	// follower 处理 Candidate 发来的投票请求 RPC
	void RequestVote(const raftRpcProctoc::RequestVoteArgs *args, raftRpcProctoc::RequestVoteReply *reply);
	
	bool UpToDate(int index, int term); // 判断候选人日志是否更新（用于投票）
	
	
	




	// 日志复制，心跳 -------------------------------------------------------------------------------------
    void leaderHearBeatTicker(); // leader 心跳定时器，周期性检查是否要发起心跳
	void doHeartBeat(); // leader 周期性主动发送心跳

	// leader 向指定节点发送追加日志RPC
	bool sendAppendEntries(int server,
						   std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> args,
						   std::shared_ptr<raftRpcProctoc::AppendEntriesReply> reply,
				 		   std::shared_ptr<int> appendNums);

	// follower 处理 leader发来的日志请求（实际处理 AppendEntries 的内部实现）
	void AppendEntries1(const raftRpcProctoc::AppendEntriesArgs* args, raftRpcProctoc::AppendEntriesReply* reply);		
	
	
	void getPrevLogInfo(int server, int *preIndex, int *preTerm); // 获取某个 Follower 上一条日志的信息，用于发送 AppendEntries
	bool matchLog(int logIndex, int logTerm); // 判断本地日志项 LogIndex 的 term 和 Leader 发来的是否匹配
	void leaderUpdateCommitIndex(); // leader 根据多数节点复制日志进度，更新提交索引 CommitIndex


	
	// 客户端命令提交 ---------------------------------------------------------------------------------
	void Start(Op command, int *newLogIndex, int *newLogTerm, bool *isLeader); 


	// ApplyMsg 推送到 KVServer ----------------------------------------------------------------------
	std::vector<ApplyMsg> getApplyLogs(); // 提取已提交但未应用的日志封装成 ApplyMsg
	void applierTicker(); 		 		  // 周期性将已提交的日志推入 applyChan 通道 （正常日志命令）
	void pushMsgToKvServer(ApplyMsg msg); // 将应用消息推送给KV服务层（快照）




	// 日志信息辅助获取 ---------------------------------------------------------------------------------
	int getLastLogIndex();  // 获取当前日志数组中最后一条日志的索引
	int getLastLogTerm();	// 获取当前日志数组中最后一条日志的任期号
	void getLastLogIndexAndTerm(int *lastLogIndex, int *lastLogTerm);// 同时获取最后一条日志的索引和任期

	int getLogTermFromLogIndex(int logIndex);	 // 获取指定 logIndex 的对应 term 

	int getSlicesIndexFromLogIndex(int logIndex);// 找到逻辑索引 logIndex 对应的物理索引 SliceIndex

	void GetState(int *term, bool *isLeader); 	 // 获取当前节点的任期和是否是Leader
	int getNewCommandIndex(); 				  	 // 获取一个新客户端命令 应该分配的逻辑日志索引（LogIndex）


	
	// 快照相关 -------------------------------------------------------------------------------------

	// 主动安装快照，抛弃旧日志
	void Snapshot(int index, std::string snapshot);

	// 条件安装快照，判断快照是否比当前状态新，决定是否安装
	bool CondInstallSnapshot(int lastIncludeTerm, int lastIncludeIndex, std::string snapshot);

	// leader 向落后follower发送快照					   
	void leaderSendSnapShot(int server);

	// 接收leader发来的快照请求，同步快照到本机（直接 RPC 调用）
	void InstallSnapshot(const raftRpcProctoc::InstallSnapshotRequest *args,
						 raftRpcProctoc::InstallSnapshotResponse *reply);



	
	// 持久化 ---------------------------------------------------------------------------------------
	void persist(); 					// 当前状态持久化 (写入磁盘)
	std::string persistData();			// 将需要持久化的状态打包为字符串（即序列化）
	void readPersist(std::string data); // 从持久化数据中恢复 Raft 状态（即反序列化）
	int GetRaftStateSize();				// 获取当前持久化状态的大小

	



	// RPC 接口重写 ------------------------------------------------------------------------------------

	// RPC接口重写，接收远程追加日志请求
	
	void AppendEntries(google::protobuf::RpcController *controller,
					   const ::raftRpcProctoc::AppendEntriesArgs *request,
					   ::raftRpcProctoc::AppendEntriesReply *response,
					   ::google::protobuf::Closure *done) override;

	// RPC接口重写，用于接收其他节点发来的 “投票请求”
	void RequestVote(google::protobuf::RpcController *controller,
					 const ::raftRpcProctoc::RequestVoteArgs *request,
					 ::raftRpcProctoc::RequestVoteReply *response,
					 ::google::protobuf::Closure *done) override;


	// RPC接口重写，接收远程快照安装请求
	void InstallSnapshot(google::protobuf::RpcController *controller,
						 const ::raftRpcProctoc::InstallSnapshotRequest *request,
						 ::raftRpcProctoc::InstallSnapshotResponse *response,
						 ::google::protobuf::Closure *done) override;





	
	




private:
	std::mutex m_mtx; // 保护 Raft 节点内部共享状态，防止多线程访问冲突

	std::vector<std::shared_ptr<RaftRpcUtil>> m_peers; // 存放所有其他节点的RPC客户端代理，方便远程调用
	std::shared_ptr<Persister> m_persister; // 指向持久化管理类，负责将核心状态保存到磁盘

	int m_me;          // 当前节点ID
	int m_currentTerm; // 当前任期号
	int m_votedFor;    // 当前任期中投票给了哪个候选人 (未投票时一般为-1)

	std::vector<raftRpcProctoc::LogEntry> m_logs; // 日志条目数组，每条日志包括 [客户端状态机命令] 和 [产生该日志的任期]

	int m_commitIndex; // 已提交日志的最大索引
	int m_lastApplied; // 已经应用到状态机的最大日志索引

	std::vector<int> m_nextIndex; // 对每个follower，leader下次要发送给它的日志索引，初始化为领导者最后日志索引 + 1。
	std::vector<int> m_matchIndex;// 对每个follower，已知和Leader同步的最新日志index

	// 节点身份枚举
	enum Status
	{
		Follower,
		Candidate,
		Leader
	};
	Status m_status; 

	// 日志应用通道 
	std::shared_ptr<LockQueue<ApplyMsg>> applyChan; // applyChan 是一个线程安全的阻塞队列，Raft将已提交日志封装为 ApplyMsg，推送到此队列
													// 应用层（如KV存储）通过从这里读取消息，应用到状态机，实现状态同步。
	// ApplyMsgQueue chan ApplyMsg // raft内部使用的chan，applyChan是用于和服务层交互，最后好像没用上

	
	// 超时管理
	std::chrono::_V2::system_clock::time_point m_lastResetElectionTime;  // 上一次重置选举超时计时器的时间点
	std::chrono::_V2::system_clock::time_point m_lastResetHeartBeatTime; // 上一次发送心跳的时间点

	// 快照相关
	int m_lastSnapshotIncludeIndex; // 快照包含的最后一个日志索引（最新）
	int m_lastSnapshotIncludeTerm;  // 对应的任期

	// 协程调度
	std::unique_ptr<monsoon::IOManager> m_ioManager = nullptr;


	// 内部私有持久化类  用于封装需持久化的数据
	class BoostPersistRaftNode 
	{
	public:
		/* BoostPersistRaftNode 提供哪些字段需要被保存 + 怎么序列化 */
		
		// 友元 （Boost 定义的特殊访问类, Boost 框架内部通过它来调用 serialize 函数）
		friend class boost::serialization::access; 

		// Boost 要求实现的序列化函数模板: 指定哪些成员变量需要持久化，如何持久化
		template <class Archive>
		void serialize(Archive &ar, const unsigned int version) 
		{
			// 这些成员变量通过 & 运算符绑定到序列化器 ar
			ar & m_currentTerm;
			ar & m_votedFor;
			ar & m_lastSnapshotIncludeIndex;
			ar & m_lastSnapshotIncludeTerm;
			ar & m_logs;

			// 当 Archive 类对应于输出档案时，& 运算符的定义类似于 <<
			// 当 Archive 类是输入档案类型时，& 运算符的定义类似于 >>
		}

		int m_currentTerm;							// 当前节点的任期（term）
		int m_votedFor;								// 当前任期中投票给了哪个候选人
		int m_lastSnapshotIncludeIndex;				// 最近一次快照中包含的最后日志索引
		int m_lastSnapshotIncludeTerm;				// 最近一次快照中包含的最后日志任期
		std::vector<std::string> m_logs;			// 当前节点保存的日志条目（字符串格式）
		std::unordered_map<std::string, int> umap;	// 哈希表，未参与序列化，可能用于辅助索引或调试
	};


};


#endif //PATH_H