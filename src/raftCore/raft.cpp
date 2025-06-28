#include "raft.h"
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <chrono>
#include <memory>
#include <ratio>
#include <unistd.h>
#include "config.h"
#include "util.h"

/**
	使服务或测试人员能够创建 Raft 服务器。
	所有 Raft 服务器（包括此服务器）的端口都在 peers[] 中。此服务器的端口为 peers[me]。所有服务器的 peers[] 数组具有相同的顺序。
	persister 是此服务器保存其持久状态的地方，并且初始保存最新保存的状态（如果有）。
	applyCh 是测试人员或服务期望 Raft 发送 ApplyMsg 消息的通道。
	Make() 必须快速返回，因此它应该为任何长时间运行的工作启动 goroutine。
*/


// 初始化Raft节点，传入集群成员代理、节点ID、持久化模块和日志应用通道
void Raft::init(std::vector<std::shared_ptr<RaftRpcUtil>> peers, 
                int me, 
                std::shared_ptr<Persister> persister,
                std::shared_ptr<LockQueue<ApplyMsg>> applyCh) 
{
	m_peers = peers;		 // 保存集群中的所有节点代理对象（RaftRpcUtil）（与其他结点沟通的rpc类）
	m_persister = persister; // 持久化类
	m_me = me; 				 // 当前节点编号（本地Raft ID）
	
	m_mtx.lock(); // 状态变量初始化过程加锁 *****************************************************************

	// applier
	this->applyChan = applyCh; // 保存应用层日志提交通道 (Raft会将已提交日志通过该队列发送给 KVServer 或上层状态机模块)
	//    rf.ApplyMsgQueue = make(chan ApplyMsg)

	m_currentTerm = 0;	// 当前任期
	m_status = Follower;// 初始staus
	m_commitIndex = 0;	// 当前已提交的日志索引
	m_lastApplied = 0;	// 当前已应用到状态机的日志索引
	m_logs.clear();		// 清空日志列表

	for (int i = 0; i < m_peers.size(); i++) 
	{
	  m_matchIndex.push_back(0); // matchIndex[i]: Leader 已知 follower 的最大匹配日志索引
	  m_nextIndex.push_back(0);	 // nextIndex[i]:  Leader 要发送给第 i 个 follower 的下一个日志索引
	}

	m_votedFor = -1; // 当前任期内投给了谁

	m_lastSnapshotIncludeIndex = 0;		// 快照包含的最新日志索引
	m_lastSnapshotIncludeTerm = 0;		// 对应的任期
	m_lastResetElectionTime = now();	// 上一次重置选举超时计时器的时间点
	m_lastResetHeartBeatTime = now();	// 上一次发送心跳的时间点

	
	readPersist(m_persister->ReadRaftState()); // 从磁盘读取上次保存的状态（即崩溃恢复）
	
	// 如果曾经应用过快照（即 snapshotIndex > 0），快照中包含了从头到 m_lastSnapshotIncludeIndex 的所有日志内容
	if (m_lastSnapshotIncludeIndex > 0) 
	{
		m_lastApplied = m_lastSnapshotIncludeIndex;
		// 既然快照中已经包含并应用了这些日志，所以恢复后无需再次 apply，从此索引继续处理。
		// 这是典型的 快照恢复行为：Raft 不会重复向状态机 apply 快照前的日志。


	  // rf.commitIndex = rf.lastSnapshotIncludeIndex   todo ：崩溃恢复为何不能读取commitIndex
	}

	// 打印调试信息（只在 Debug 模式下生效）
  	DPrintf("[Init&ReInit] Sever %d, term %d, lastSnapshotIncludeIndex {%d} , lastSnapshotIncludeTerm {%d}", 
			m_me, m_currentTerm, m_lastSnapshotIncludeIndex, m_lastSnapshotIncludeTerm);

  	m_mtx.unlock(); // 解锁 *******************************************************************************

	
	// 初始化协程调度器  
  	m_ioManager = std::make_unique<monsoon::IOManager>(FIBER_THREAD_NUM, 		// 协程调度线程数
													   FIBER_USE_CALLER_THREAD);// 当前线程是否作为调度线程之一


	// start ticker 启动三个循环定时器
	m_ioManager->scheduler([this]() -> void { this->leaderHearBeatTicker(); });
	m_ioManager->scheduler([this]() -> void { this->electionTimeOutTicker(); });

	std::thread t3(&Raft::applierTicker, this);
	t3.detach();

	// todo:原来是启动了三个线程，现在是直接使用了协程，
	// 三个函数中leaderHearBeatTicker、electionTimeOutTicker执行时间是恒定的，
	// applierTicker时间受到数据库响应延迟和两次apply之间请求数量的影响，这个随着数据量增多可能不太合理，最好其还是启用一个线程。

	// std::thread t(&Raft::leaderHearBeatTicker, this);
	// t.detach();
	//
	// std::thread t2(&Raft::electionTimeOutTicker, this);
	// t2.detach();
	//
	// std::thread t3(&Raft::applierTicker, this);
	// t3.detach();
}



// 选举定时器：周期性检查选举是否超时，触发新一轮选举
void Raft::electionTimeOutTicker() 
{
	/*
		当当前节点不是 Leader 且超过一定时间未收到心跳，则发起新一轮选举（Candidate 状态）
	*/

	
	while (true) // 无限循环，不断检测是否需要发起选举
	{
		
		// Leader 不需要参与选举，所以这里 sleep 一小段时间避免空转（浪费 CPU）
		while (m_status == Leader)
		{		
			usleep(HeartBeatTimeout); 
			// 定时时间没有严谨设置，因为HeartBeatTimeout比选举超时一般小一个数量级，经验值，睡个合适的时间段都行

			/**
			* 如果不睡眠，那么对于leader，这个函数会一直空转，浪费cpu
			* 且加入协程之后，空转会导致其他协程无法运行，对于时间敏感的AE，会导致心跳无法正常发送导致异常
			*/	
		}

		
		std::chrono::duration<signed long int, std::ratio<1, 1000000000>> suitableSleepTime{};// 适合的睡眠时间
		std::chrono::system_clock::time_point wakeTime{};// 唤醒时间点

		{
			m_mtx.lock();
			wakeTime = now(); 
			// 距离下一次超时应该睡眠的时间 = 上次重置选举超时计时器的时间 + 随机化的选举超时时间 - 唤醒时间点
			suitableSleepTime = m_lastResetElectionTime + getRandomizedElectionTimeout() - wakeTime;
			m_mtx.unlock();
		}

		// 如果应该睡眠时间大于 1 毫秒，就 sleep，避免睡眠太短而频繁切换线程
		if (std::chrono::duration<double, std::milli>(suitableSleepTime).count() > 1)
		{
			auto start = std::chrono::steady_clock::now(); // 获取当前时间点

			usleep(std::chrono::duration_cast<std::chrono::microseconds>(suitableSleepTime).count());
			// std::this_thread::sleep_for(suitableSleepTime);

			auto end = std::chrono::steady_clock::now(); // 获取函数运行结束后的时间点

			// 计算时间差，记录sleep的实际时间（单位为毫秒）
			std::chrono::duration<double, std::milli> duration = end - start;

			// 输出，使用ANSI控制序列将输出颜色改为紫色
			std::cout << "\033[1;35m electionTimeOutTicker();函数设置睡眠时间为："
					  << std::chrono::duration_cast<std::chrono::milliseconds>(suitableSleepTime).count() 
					  << "毫秒\033[0m" 
					  << std::endl;
			
			std::cout << "\033[1;35m electionTimeOutTicker();函数实际睡眠时间为："
					  << duration.count()
					  << "毫秒\033[0m"
					  << std::endl;
		
		}

		// 如果这段时间内 m_lastResetElectionTime 被重置，说明 Leader 发来了心跳，不能选举，继续等待
		if (std::chrono::duration<double, std::milli>(m_lastResetElectionTime - wakeTime).count() > 0)
		{
			// 每次 AppendEntries 成功处理（Leader 心跳）时都会更新 m_lastResetElectionTime，从而推迟选举
			// 如果睡眠的这段时间有重置定时器，那么就没有超时，再次睡眠
			continue;
		}

		doElection();

  	}
}














void Raft::AppendEntries1(const raftRpcProctoc::AppendEntriesArgs* args, raftRpcProctoc::AppendEntriesReply* reply) 
{
  
}


void Raft::applierTicker() 
{
  
  
}


bool Raft::CondInstallSnapshot(int lastIncludedTerm, int lastIncludedIndex, std::string snapshot) 
{
  
}


void Raft::doElection() 
{
  
}


void Raft::doHeartBeat() 
{
  
}




std::vector<ApplyMsg> Raft::getApplyLogs() 
{
  

}



// 获取新命令应该分配的Index
int Raft::getNewCommandIndex() 
{
  
}



// getPrevLogInfo
// leader调用，传入：服务器index，传出：发送的AE的preLogIndex和PrevLogTerm
void Raft::getPrevLogInfo(int server, int* preIndex, int* preTerm) 
{
  
}



// GetState return currentTerm and whether this server
// believes it is the Leader.
void Raft::GetState(int* term, bool* isLeader) 
{
  
}


void Raft::InstallSnapshot(const raftRpcProctoc::InstallSnapshotRequest* args,
                           raftRpcProctoc::InstallSnapshotResponse* reply) 
{
  
  
}


void Raft::pushMsgToKvServer(ApplyMsg msg) 
{ 
	
}



void Raft::leaderHearBeatTicker() 
{
  
	
}


void Raft::leaderSendSnapShot(int server) 
{
  
   
}


void Raft::leaderUpdateCommitIndex() 
{
 
}


//进来前要保证logIndex是存在的，即≥rf.lastSnapshotIncludeIndex	，而且小于等于rf.getLastLogIndex()
bool Raft::matchLog(int logIndex, int logTerm) 
{
 
}


void Raft::persist() 
{
  
}


void Raft::RequestVote(const raftRpcProctoc::RequestVoteArgs* args, raftRpcProctoc::RequestVoteReply* reply) 
{
  
}


bool Raft::UpToDate(int index, int term) 
{
  
}


void Raft::getLastLogIndexAndTerm(int* lastLogIndex, int* lastLogTerm) 
{
  
}

/**
 *
 * @return 最新的log的logindex，即log的逻辑index。区别于log在m_logs中的物理index
 * 可见：getLastLogIndexAndTerm()
 */

int Raft::getLastLogIndex() 
{
  
}


int Raft::getLastLogTerm() 
{
  
}

/**
 *
 * @param logIndex log的逻辑index。注意区别于m_logs的物理index
 * @return
 */

int Raft::getLogTermFromLogIndex(int logIndex) 
{

}


int Raft::GetRaftStateSize() 
{ 
	return m_persister->RaftStateSize(); 
}



// 找到index对应的真实下标位置！！！
// 限制，输入的logIndex必须保存在当前的logs里面（不包含snapshot）
int Raft::getSlicesIndexFromLogIndex(int logIndex) 
{

}



bool Raft::sendRequestVote(int server, std::shared_ptr<raftRpcProctoc::RequestVoteArgs> args,
                           std::shared_ptr<raftRpcProctoc::RequestVoteReply> reply, std::shared_ptr<int> votedNum) 
{

}



bool Raft::sendAppendEntries(int server, std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> args,
                             std::shared_ptr<raftRpcProctoc::AppendEntriesReply> reply,
                             std::shared_ptr<int> appendNums) 
{
  
}



void Raft::AppendEntries(google::protobuf::RpcController* controller,
                         const ::raftRpcProctoc::AppendEntriesArgs* request,
                         ::raftRpcProctoc::AppendEntriesReply* response, ::google::protobuf::Closure* done) 
{

}



void Raft::InstallSnapshot(google::protobuf::RpcController* controller,
                           const ::raftRpcProctoc::InstallSnapshotRequest* request,
                           ::raftRpcProctoc::InstallSnapshotResponse* response, ::google::protobuf::Closure* done) 
{

}



void Raft::RequestVote(google::protobuf::RpcController* controller, const ::raftRpcProctoc::RequestVoteArgs* request,
                       ::raftRpcProctoc::RequestVoteReply* response, ::google::protobuf::Closure* done) 
{

}



void Raft::Start(Op command, int* newLogIndex, int* newLogTerm, bool* isLeader) 
{

}



std::string Raft::persistData() 
{
  
}



void Raft::readPersist(std::string data) 
{
  
}




void Raft::Snapshot(int index, std::string snapshot) 
{
  
}