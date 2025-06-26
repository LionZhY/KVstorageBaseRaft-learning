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
	// 初始化和持久化 -------------------------------------------------------------------------------------

	// 初始化Raft节点，传入集群成员代理、节点ID、持久化模块和日志应用通道
	void init(std::vector<std::shared_ptr<RaftRpcUtil>> peers, int me,
			  std::shared_ptr<Persister> persister,
			  std::shared_ptr<LockQueue<ApplyMsg>> applyCh);
	
	// 当前状态持久化到磁盘
	void persist();
	// 读取持久化数据，恢复状态
	void readPersist(std::string data);
	// 获取当前状态序列化后的字符串，供持久化存储
	std::string persistData();

	// 获取当前持久化状态的大小
	int GetRaftStateSize();



	// 日志信息辅助获取 --------------------------------------------------------------------------------------
	int getLastLogIndex();  // 获取当前日志数组中最后一条日志的索引
	int getLastLogTerm();	// 获取当前日志数组中最后一条日志的任期号
	void getLastLogIndexAndTerm(int *lastLogIndex, int *lastLogTerm);// 同时获取最后一条日志的索引和任期
	int getLogTermFromLogIndex(int logIndex);	 // 根据给定的日志下标获取其对应的任期
	int getSlicesIndexFromLogIndex(int logIndex);// 将逻辑日志索引转换为数组中的切片索引



	// 客户端交互与状态查询 ----------------------------------------------------------------------------

	// 外部接口，客户端调用提交新的命令
	void Start(Op command, int *newLogIndex, int *newLogTerm, bool *isLeader);

	// 获取当前可以追加新日志的索引位置
	int getNewCommanIndex();

	// 获取待应用日志列表
	std::vector<ApplyMsg> getApplyLogs();

	// 获取当前节点的任期
	void GetState(int *term, bool *isLeader);

	// 将应用消息推送给KV服务层
	void pushMsgToKvServer(ApplyMsg msg);


	// 选举流程 -------------------------------------------------------------------------------------

	// 发起一次选举
	void doElection(); 
	
	// 周期性检查选举是否超时
	void electionTimeOutTicker();
	
	// 处理投票请求RPC
	void RequestVote(const raftRpcProctoc::RequestVoteArgs *args, raftRpcProctoc::RequestVoteReply *reply);
	
	// 作为候选者，发送请求投票RPC给指定服务器
	bool sendRequestVote(int server,
				 	 std::shared_ptr<raftRpcProctoc::RequestVoteArgs> args,
				 	 std::shared_ptr<raftRpcProctoc::RequestVoteReply> reply,
				 	 std::shared_ptr<int> votedNum);
	
					 // 判断另一个节点（如候选人）的日志是否“足够新”，用于投票判断是否接受候选人
	bool UpToDate(int index, int term);



	// 日志复制与心跳机制 ------------------------------------------------------------------------------

	// leader周期性发送心跳
	void doHeartBeat();

	// leader 心跳定时器
	void leaderHearBeatTicker();

	// 内部处理追加日志请求 （实际处理 AppendEntries 的内部实现）
	void AppendEntries1(const raftRpcProctoc::AppendEntriesArgs* args, raftRpcProctoc::AppendEntriesReply* reply);
	
	// 获取某个 Follower 上一条日志的信息，用于发送 AppendEntries
	void getPrevLogInfo(int server, int *preIndex, int *preTerm);

	// leader 向指定节点发送追加日志RPC
	bool sendAppendEntries(int server,
						   std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> args,
						   std::shared_ptr<raftRpcProctoc::AppendEntriesReply> reply,
				 		   std::shared_ptr<int> appendNums);


	// 判断本地日志指定位置和任期是否匹配，用于日志一致性检测
	bool matchLog(int logIndex, int logTerm);

	// leader 根据多数节点复制日志进度，更新提交索引
	void leaderUpdateCommitIndex();






	// 快照相关 -------------------------------------------------------------------------------------

	// 上层服务通知Raft快照数据及应用到哪个日志索引，Raft截断日志，保存快照
	void Snapshot(int index, std::string snapshot);

	// 条件安装快照，判断快照是否比当前状态新，决定是否安装
	bool CondInstallSnapshot(int lastIncludeTerm, int lastIncludeIndex, std::string snapshot);

	// leader 向落后follower发送快照，防止日志复制因差距过大失败
	void leaderSendSnapShot(int server);



	// 处理快照安装RPC请求
	void InstallSnapshot(const raftRpcProctoc::InstallSnapshotRequest *args,
						 raftRpcProctoc::InstallSnapshotResponse *reply);


	// Snapshot the service says it has created a snapshot that has
	// all info up to and including index. this means the
	// service no longer needs the log through (and including)
	// that index. Raft should now trim its log as much as possible.
	// index代表是快照apply应用的index,而snapshot代表的是上层service传来的快照字节流，包括了Index之前的数据
	// 这个函数的目的是把安装到快照里的日志抛弃，并安装快照数据，同时更新快照下标，属于peers自身主动更新，与leader发送快照不冲突
	// 即服务层主动发起请求raft保存snapshot里面的数据，index是用来表示snapshot快照执行到了哪条命令
	



	// RPC 接口 -------------------------------------------------------------------------------------

	// RPC接口重写，接收远程追加日志请求
	// 重写基类方法,因为rpc远程调用真正调用的是这个方法
	// 序列化，反序列化等操作rpc框架都已经做完了，因此这里只需要获取值然后真正调用本地方法即可。
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




	// 日志应用相关 -------------------------------------------------------------------------------------
	
	// 独立线程或协程定时调用，应用已提交日志到状态机
	void applierTicker();



private:
	std::mutex m_mtx; // 保护 Raft 节点内部共享状态，防止多线程访问冲突

	std::vector<std::shared_ptr<RaftRpcUtil>> m_peers; // 存放所有其他节点的RPC客户端代理，方便远程调用
	std::shared_ptr<Persister> m_persister; // 指向持久化管理类，负责将核心状态保存到磁盘

	int m_me;          // 当前节点ID
	int m_currentTerm; // 当前任期号
	int m_votedFor;    // 本任期已投票的候选人ID (未投票时一般为-1)

	std::vector<raftRpcProctoc::LogEntry> m_logs; // 日志条目数组，每条日志包括 [客户端状态机命令] 和 [产生该日志的任期]

	int m_commitIndex; // 已提交日志的最大索引
	int m_lastApplied; // 已经应用到状态机的最大日志索引

	std::vector<int> m_nextIndex; // 对每个follower，leader下次要发送给它的日志索引，初始化为领导者最后日志索引 + 1。
	std::vector<int> m_matchIndex;// 对每个follower，已知其匹配成功的最大日志索引

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
	int m_lastSnapshotIncludeIndex; // 快照包含的最新日志索引
	int m_lastSnapshotIncludeTerm;  // 对应的任期

	// 协程调度
	std::unique_ptr<monsoon::IOManager> m_ioManager = nullptr;


	// 内部持久化类  配合 Boost 序列化，保存当前 Term、投票信息、日志内容等状态
	class BoostPersistRaftNode 
	{
	public:
		friend class boost::serialization::access;
		// When the class Archive corresponds to an output archive, the
		// & operator is defined similar to <<.  Likewise, when the class Archive
		// is a type of input archive the & operator is defined similar to >>.
		template <class Archive>
		void serialize(Archive &ar, const unsigned int version) 
		{
			ar & m_currentTerm;
			ar & m_votedFor;
			ar & m_lastSnapshotIncludeIndex;
			ar & m_lastSnapshotIncludeTerm;
			ar & m_logs;
		}
		int m_currentTerm;
		int m_votedFor;
		int m_lastSnapshotIncludeIndex;
		int m_lastSnapshotIncludeTerm;
		std::vector<std::string> m_logs;
		std::unordered_map<std::string, int> umap;

	};


};



#endif //PATH_H