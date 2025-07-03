#include "raft.h"
#include <algorithm>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <ratio>
#include <sys/ioctl.h>
#include <unistd.h>
#include "config.h"
#include "raftRPC.pb.h"
#include "util.h"

/**
	使服务或测试人员能够创建 Raft 服务器。
	所有 Raft 服务器（包括此服务器）的端口都在 peers[] 中。此服务器的端口为 peers[me]。所有服务器的 peers[] 数组具有相同的顺序。
	persister 是此服务器保存其持久状态的地方，并且初始保存最新保存的状态（如果有）。
	applyCh 是测试人员或服务期望 Raft 发送 ApplyMsg 消息的通道。
	Make() 必须快速返回，因此它应该为任何长时间运行的工作启动 goroutine。
*/


/////////////////////////////////////////////  leader 选举  /////////////////////////////////////////////

// 初始化Raft节点，传入集群成员代理、节点ID、持久化模块和日志应用通道
void Raft::init(std::vector<std::shared_ptr<RaftRpcUtil>> peers, 
                int me, 
                std::shared_ptr<Persister> persister,
                std::shared_ptr<LockQueue<ApplyMsg>> applyCh) 
{
	m_peers = peers;		 // 保存集群中的所有节点代理对象（RaftRpcUtil）（与其他结点沟通的rpc类）
	m_persister = persister; // 持久化类
	m_me = me; 				 // 当前节点编号（本地Raft ID）
	
	m_mtx.lock(); // 状态变量初始化过程加锁

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

  	m_mtx.unlock(); 

	
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

		// 计算当前节点可以睡多久再醒来，判断是否超时开始选举
		std::chrono::duration<signed long int, std::ratio<1, 1000000000>> suitableSleepTime{};// 适合的睡眠时间
		std::chrono::system_clock::time_point wakeTime{};// 唤醒时间点

		{
			m_mtx.lock();
			wakeTime = now(); 
			// 距离下一次超时可以睡眠的时间 = 上次重置选举超时计时器的时间 + 随机化的选举超时时间 - 唤醒时间点
			suitableSleepTime = m_lastResetElectionTime + getRandomizedElectionTimeout() - wakeTime;
			m_mtx.unlock();
		}

		// 根据 suitableSleepTime 判断是否需要 sleep
		if (std::chrono::duration<double, std::milli>(suitableSleepTime).count() > 1) // 转换成单位为 毫秒
		{
			/* 如果应该睡眠时间大于 1 毫秒，就 sleep，避免睡眠太短而频繁切换线程 */

			auto start = std::chrono::steady_clock::now(); // 获取当前时间点，表示 sleep 之前的“起点”

			// 开始睡眠
			// usleep(std::chrono::duration_cast<std::chrono::microseconds>(suitableSleepTime).count()); // 转换成微秒
			std::this_thread::sleep_for(suitableSleepTime); // 也可以用这种形式，更现代安全

			auto end = std::chrono::steady_clock::now();   // sleep 结束时间点


			// 计算 & 打印 sleep 实际耗时（单位为毫秒）
			std::chrono::duration<double, std::milli> duration = end - start;
			// 输出，使用ANSI控制序列将输出颜色改为紫色
			std::cout << "\033[1;35m electionTimeOutTicker();函数设置睡眠时间为：" // 期望值
					  << std::chrono::duration_cast<std::chrono::milliseconds>(suitableSleepTime).count() 
					  << "毫秒\033[0m" 
					  << std::endl;
			
			std::cout << "\033[1;35m electionTimeOutTicker();函数实际睡眠时间为：" // 实际睡眠时间可能略大
					  << duration.count()
					  << "毫秒\033[0m"
					  << std::endl;
		}

		// “睡眠期间” 收到 Leader 的心跳（m_lastResetElectionTime 被更新），跳过选举
		if (std::chrono::duration<double, std::milli>(m_lastResetElectionTime - wakeTime).count() > 0)
		{
			// 每次 AppendEntries 成功处理（Leader 心跳）时都会更新 m_lastResetElectionTime
			
			// 比较当前时间点 wakeTime 和 m_lastResetElectionTime
			// 如果 m_lastResetElectionTime 更晚，说明m_lastResetElectionTime 就被更新了，这段睡眠期间收到了 Leader 心跳
			// 收到心跳，就跳过选举。continue 重新进入 while 循环，重新计算下一次睡眠时间
			continue;
		}

		// 发起选举
		doElection();
  	}
}



// 发起选举  （Candidate 角色的主要职责）
void Raft::doElection() 
{
	std::lock_guard<std::mutex> g(m_mtx); // 加锁

	// 只有在当前节点不是leader的情况下，才允许转为 Candidate 发起选举
	if (m_status != Leader)
	{
		DPrintf("[       ticker-func-rf(%d)              ]  选举定时器到期且不是leader，开始选举 \n", m_me);

		m_status = Candidate; 	// 变更状态为 Candidate
		m_currentTerm += 1;		// 任期 + 1 （无论是刚开始竞选，还是超时重新竞选，term都要增加）
		m_votedFor = m_me; 		// 给自己投，也避免candidate给同辈的candidate投
		persist();				// 持久化当前的 term 和 voteFor

		// 使用共享指针计数投票数，初始化为1（表示自己投自己的一票）
		std::shared_ptr<int> votedNum = std::make_shared<int>(1); // 使用 make_shared 函数初始化 ！！ 亮点

		// 更新选举超时定时器，避免在发起投票后马上又触发下一轮选举
		m_lastResetElectionTime = now();

		
		// 向其他集群节点发送投票请求 RequestVote RPC 
		for (int i = 0; i < m_peers.size(); i++)
		{
			if (i == m_me)	continue; // 跳过自己

			int lastLogIndex = -1, lastLogTerm = -1;
			getLastLogIndexAndTerm(&lastLogIndex, &lastLogTerm); // 获取当前节点的最后一条日志的 index 和 term

			// 构造 RequestVote RPC 请求
			std::shared_ptr<raftRpcProctoc::RequestVoteArgs> requestVoteArgs = 
							std::make_shared<raftRpcProctoc::RequestVoteArgs>();
			requestVoteArgs->set_term(m_currentTerm);
			requestVoteArgs->set_candidateid(m_me);
			requestVoteArgs->set_lastlogindex(lastLogIndex);
			requestVoteArgs->set_lastlogterm(lastLogTerm);

			// 创建 RPC 响应对象（占位，后续在子线程中填充）
			auto requestVoteReply = std::make_shared<raftRpcProctoc::RequestVoteReply>();

			// 启动一个新线程异步向节点 i 发送 RequestVote RPC， 避免在持锁状态下执行 RPC（防止阻塞其他线程）
			std::thread t(&Raft::sendRequestVote, this, i, requestVoteArgs, requestVoteReply, votedNum); // 创建新线程并执行b函数，并传递参数
			t.detach();
		}
	}
}



// candidate 发送请求投票RPC给指定节点
bool Raft::sendRequestVote(int server, 												// 目标节点下标
						   std::shared_ptr<raftRpcProctoc::RequestVoteArgs> args,	// RPC请求参数（term，候选人id，最后日志）
                           std::shared_ptr<raftRpcProctoc::RequestVoteReply> reply, // RPC响应结果
						   std::shared_ptr<int> votedNum) 							// 记录当前获得投票数的共享指针
{
	
	auto start = now(); // 记录耗时
	DPrintf("[func-sendRequestVote rf{%d}] 向server{%d} 发送 RequestVote 开始", 
			 m_me, m_currentTerm, getLastLogIndex());

	// 调用远程代理对象 m_peers[server]->RequestVote() 发起实际的投票请求 -----------------------------------
	bool ok = m_peers[server]->RequestVote(args.get(), reply.get()); // ok 仅表示网络通信是否成功，不代表投票是否成功
	DPrintf("[func-sendRequestVote rf{%d}] 向server{%d} 发送 RequestVote 完毕，耗时:{%d} ms", 
			 m_me, m_currentTerm, getLastLogIndex(), now() - start);

	// rpc通信失败就立即返回，避免资源消耗
	if (!ok)
	{
		return ok; //不知道为什么不加这个的话如果服务器宕机会出现问题的，通不过2B  todo
  	}

	// 通信ok 进入临界区处理RPC响应结果 --------------------------------------------------------------------
	std::lock_guard<std::mutex> lg(m_mtx);
	
	if (reply->term() > m_currentTerm) 		// reply.term > 当前任期，说明自己已经过期，转为 Follower 
	{
		m_status = Follower;
		m_currentTerm = reply->term();
		m_votedFor = -1;
		persist();
		return true;
	}
	else if (reply->term() < m_currentTerm) // reply.term < 当前任期：term过期恢复，忽略该投票，返回
	{
		return true; 
	}

	// 断言 reply.term == 当前任期
	myAssert(reply->term() == m_currentTerm, format("assert {reply.Term==rf.currentTerm} fail"));
	
	// reply.term == 当前任期，处理投票结果 ---------------------------------------------------------------
	if (!reply->votegranted()) 				
	{
		return true; // 如果没投票给我，忽略
	}
	
	// 收到投票，更新 votedNum
	*votedNum = *votedNum + 1; 
	if (*votedNum >= m_peers.size() / 2 + 1) // 若获得超过半数票，成为 Leader ！！！！！！-------------------
	{
		*votedNum = 0;

		// 如果已经是leader了，那么不会进行下一步处理
		if (m_status == Leader) 
		{
			myAssert(false, format("[func-sendRequestVote-rf{%d}]  term:{%d} 同一个term当两次领导，error", m_me, m_currentTerm));
		}

		// 第一次变成leader，改身份，初始化nextIndex 和 matchIndex，通知其他节点 -----------------------------
		m_status = Leader; 							 
		DPrintf("[func-sendRequestVote rf{%d}] elect success  ,current term:{%d} ,lastLogIndex:{%d}\n",
				 m_me, m_currentTerm, getLastLogIndex());
		
		int lastLogIndex = getLastLogIndex(); 
		for (int i = 0; i < m_nextIndex.size(); i++) 
		{
			m_nextIndex[i] = lastLogIndex + 1; // 有效下标从1开始，因此要+1
			m_matchIndex[i] = 0;
		}

		// 异步启动 doHeartBeat()，马上通知其他节点自己就是新leader
		std::thread t(&Raft::doHeartBeat, this); 
		t.detach();

		// 持久化最新状态
		persist(); 
	}

	return true; // 返回true 只表示RPC通信成功（不包括投票状态）
}



// follower 处理 Candidate 发来的投票请求 RPC
void Raft::RequestVote (const raftRpcProctoc::RequestVoteArgs *args, // 请求投票RPC参数结构(candidate发来的)
						raftRpcProctoc::RequestVoteReply *reply)	 // 当前节点的投票回复RPC参数结构
{
	std::lock_guard<std::mutex> lg(m_mtx); 
	DEFER // 延迟执行，持久化保存当前状态
	{
		persist(); //应该先持久化，再撤销lock，所以写在lock后面
	};

	/*	投票的三大条件：
		1 候选人的 term ≥ 我当前 term （比我新）
		2 候选人的日志不比我旧 （保证leader日志完整性）
		3 我还没投票或投给了同一个候选人 （一个任期内只能投一票）
	*/

	// ------------------------------------------  term 新 ------------------------------------------
	// 对方任期小于我 → 拒绝投票（过时）
	if (args->term() < m_currentTerm)
	{
		reply->set_term(m_currentTerm);	// 设置响应中的term (告诉候选人我的term比较新，你应该更新你的term)
		reply->set_votestate(Expire);	// 投票状态标记：过期
		reply->set_votegranted(false);	// 告诉候选人，拒绝投票
		return;
	}

	// 对方任期更大 → 更新自己的 term + 退回 Follower
	if (args->term() > m_currentTerm)
	{
		/* Raft 协议规定：只要收到更高的 term，就必须更新自己的 term，并退回 Follower 状态 */
		m_status = Follower;
		m_currentTerm = args->term(); 
		m_votedFor = -1; // 新任期，重置投票记录（刚才的任期内已经投给了现在这个Candidate，现在是新任期）
	}

	// 断言：节点相同
	myAssert(args->term() == m_currentTerm,
			 format("[func--rf{%d}] 前面校验过args.Term==rf.currentTerm，这里却不等", m_me));
	

	// ----------------------------------------- log 不落后-----------------------------------------

	// 现在节点任期都是相同的 (前面已经处理了不同任期的，任期小的也已经更新到新的 args 的 term 了)
	// 还需要检查 log 的 term 和 index 是不是匹配的

	// 日志是否 “更新” → 决定是否有资格投票

	int lastLogTerm = getLastLogTerm();

	// 没投票，且candidate的日志的新的程度 >= 接收者的日志新的程度，才会投票
	if (!UpToDate(args->lastlogindex(), args->lastlogterm())) 
	{
    	// 日志不够新，拒绝投票
		reply->set_term(m_currentTerm);
    	reply->set_votestate(Voted);	// 拒绝投票的状态码（用于调试或日志）
    	reply->set_votegranted(false);	// 拒绝投票
    	return;
	}	 


	// ------------------------------------ 一个term内，投一次票 ------------------------------------

	// 是否已经投过票 → 限制一个term内 只投一次票
	if (m_votedFor != -1 && m_votedFor != args->candidateid())
	{
		// 已经投过票，但当前请求不是我投过票的人，拒绝这次投票

		// m_votedFor != args->candidateid() 如果是我刚投过的候选人，会进入下面else，再次确认投票，这是允许的
		// 网络丢包/重传场景 —— 候选人可能因超时或网络不稳定重新发送 RequestVote 请求

		reply->set_term(m_currentTerm);
		reply->set_votestate(Voted);	
		reply->set_votegranted(false);	
		return;
	}
	else // 满足所有条件 → 投票通过
	{
		m_votedFor = args->candidateid(); // 记录投票对象，防止重复投
		m_lastResetElectionTime = now();  // 重置选举计时器，避免误判超时

		reply->set_term(m_currentTerm);
		reply->set_votestate(Normal);	  // 投票正常
		reply->set_votegranted(true);	  // 同意投票
		return;
	}
	
}


// 判断候选人日志是否不比自己旧（用于投票）
bool Raft::UpToDate(int index, int term) // 候选人最后一条日志的 Term 和 index
{
	/*
	判断日志 “新不新” 的标准是：
		先比较最后一条日志的 Term
		如果 Term 相同，再比较日志索引（Index）
	*/
	int lastIndex = -1; 
	int lastTerm = -1;
	getLastLogIndexAndTerm(&lastIndex, &lastTerm); // 当前节点（即 Follower）自己维护的最后一条日志的 Term 和 Index
	
	// 候选人term更新，或者term相同但候选人的日志索引不比自己落后，都可以投票
	return term > lastTerm || (term == lastTerm && index >= lastIndex);
}



/////////////////////////////////////////////  日志复制 心跳  /////////////////////////////////////////////

// leader 心跳定时器，周期性检查是否要发起心跳
void Raft::leaderHearBeatTicker() 
{
	/* 定期向所有 Follower 发送心跳（即 AppendEntries RPC）以维持 Leader 身份 */

	while (true) // 一旦成为 Leader，就一直循环判断是否该发送心跳
	{
		// 当前节点不是leader，进入暂时休眠
		while (m_status != Leader)
		{
			usleep(1000 * HeartBeatTimeout);
		}

		static std::atomic<int32_t> atomicCount = 0; // 原子计数器，记录心跳发送次数，用于调试打印

		
		// 计算 “下一次” 心跳前还需等待多久
		std::chrono::duration<signed long int, std::ratio<1, 1000000000>> suitableSleepTime{};
		std::chrono::system_clock::time_point wakeTime{};

		{
			std::lock_guard<std::mutex> lock(m_mtx);
			wakeTime = now();
			// 距离下次心跳还有多久 = (上次发心跳的时间 + 心跳间隔) - 当前时间
			suitableSleepTime = std::chrono::milliseconds(HeartBeatTimeout) + m_lastResetHeartBeatTime - wakeTime;
		}


		// 如果当前距离下一次心跳还有较长时间（>1ms），sleep 等待，避免频繁切线程
		if (std::chrono::duration<double, std::milli>(suitableSleepTime).count() > 1)
		{
			std::cout << atomicCount << "\033[1;35m leaderHearBeatTicker();函数设置睡眠时间为: "
					  << std::chrono::duration_cast<std::chrono::milliseconds>(suitableSleepTime).count() << "毫秒\033[0m"
					  << std::endl;
			
			auto start = std::chrono::steady_clock::now(); // 获取当前时间点

			std::this_thread::sleep_for(suitableSleepTime);
			// usleep(std::chrono::duration_cast<std::chrono::microseconds>(suitableSleepTime).count());

			auto end = std::chrono::steady_clock::now(); // 获取睡眠结束后的时间点

			// 计算sleep时长并打印（毫秒）
			std::chrono::duration<double, std::milli> duration = end - start;

			std::cout << atomicCount << "\033[1;35m leaderHearBeatTicker();函数实际睡眠时间为: " 
					  << duration.count() << " 毫秒\033[0m" << std::endl;
			
			// 心跳次数 + 1
			++atomicCount; 
		}

		// 如果sleep期间定时器被重置了，就再次等待（不用发心跳）
		if (std::chrono::duration<double, std::milli>(m_lastResetHeartBeatTime - wakeTime).count() > 0)
		{
			// 说明在 sleep 期间，其他线程已经重置了 m_lastResetHearBeatTime（即这个 Leader 在其他线程发过心跳了）
			continue;
		}


		// 发送心跳
		doHeartBeat();

	}

}


// leader 周期性主动发送心跳
void Raft::doHeartBeat() 
{
	std::lock_guard<std::mutex> lg(m_mtx);

	if (m_status == Leader) // 确保当前节点是Leader才执行心跳发送
	{
		DPrintf("[func-Raft::doHeartBeat()-Leader: {%d}] Leader的心跳定时器触发了且拿到mutex，开始发送AE\n", m_me);
		
		auto appendNums = std::make_shared<int>(1); // 统计有多少节点成功响应心跳（初始认为Leader自己成功一票）

		
    	// todo 这里肯定是要修改的，最好使用一个单独的goruntime来负责管理发送log，因为后面的log发送涉及优化之类的
    	// 最少要单独写一个函数来管理，而不是在这一坨

		// 遍历所有Follower（向除了自己外的所有节点发送AE）
		for (int i = 0; i < m_peers.size(); i++)
		{
			if (i == m_me) // 跳过自己
			{
				continue;
			}

			// 打印每一个发送目标的编号
			DPrintf("[func-Raft::doHeartBeat()-Leader: {%d}] Leader的心跳定时器触发了 index:{%d}\n", m_me, i);
			// 断言保证 每个 Follower 的 nextIndex（下次应发送的日志索引）必须合法
			myAssert(m_nextIndex[i] >= 1, format("rf.nextIndex[%d] = {%d}", i, m_nextIndex[i]));

			// 判断是否需要发送快照 (若 nextIndex 已被压缩)
			if (m_nextIndex[i] <= m_lastSnapshotIncludeIndex)
			{
				// 如果 nextIndex <= 快照起始点，说明该 Follower 已落后太久，当前 Leader 日志中不再包含这么早的数据（已被截断）
				std::thread t(&Raft::leaderSendSnapShot, this, i); // 启动一个线程，发送快照，代替AE
				t.detach();
				continue;
			}

			/* 下面是 m_nextIndex[i] > m_lastSnapshotIncludeIndex 的情况*/


			// Leader 想要发送给 Follower 的日志条目中，新日志的前一条日志的索引 (preLogIndex = m_nextIndex - 1)
			int preLogIndex = -1; 
			int preLogTerm = -1;  
			getPrevLogInfo(i, &preLogIndex, &preLogTerm);
			
			
			// 构造 AppendEntries RPC 参数结构
			std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> appendEntriesArgs = std::make_shared<raftRpcProctoc::AppendEntriesArgs>();
			appendEntriesArgs->set_term(m_currentTerm);
			appendEntriesArgs->set_leaderid(m_me);
			appendEntriesArgs->set_prevlogindex(preLogIndex);
			appendEntriesArgs->set_prevlogterm(preLogTerm);
			appendEntriesArgs->set_leadercommit(m_commitIndex);
			appendEntriesArgs->clear_entries(); // 清空日志条目列表，准备添加
			

			// 判断是直接从m_logs 里 preLogIndex + 1 开始发，还是直接发送全部 m_logs
			if (preLogIndex != m_lastSnapshotIncludeIndex) 
			{
				// preLogIndex 是出现在日志数组 m_logs 中的（不是快照里），说明它可以从 m_logs 中继续发日志
				for (int j = getSlicesIndexFromLogIndex(preLogIndex) + 1; j < m_logs.size(); ++j) // 索引转换 getSlicesIndexFromLogIndex()
				{
					// 从 preLogIndex + 1 开始，把所有后续日志 m_logs[j] 打包到发送日志 entries 里
					raftRpcProctoc::LogEntry* sendEntryPtr = appendEntriesArgs->add_entries();
					*sendEntryPtr = m_logs[j];
				}
			}
			else 
			{	
				// 如果 preLogIndex == m_lastSnapshotIncludeIndex， preLogIndex就是在快照中的最后一条
				// Leader 只能把 m_logs 中所有日志打包发送
				for (const auto& item : m_logs)
				{
					raftRpcProctoc::LogEntry* sendEntryPtr = appendEntriesArgs->add_entries();
					*sendEntryPtr = item;
				}
			}

			
			int lastLogIndex = getLastLogIndex(); // 当前 Leader 最后一条日志的索引值

			// Leader请求前本地断言：发送的日志前一条的日志索引 + 要发送的日志条目数量 = 当前日志末尾
			myAssert(appendEntriesArgs->prevlogindex() + appendEntriesArgs->entries_size() == lastLogIndex,
					 format("appendEntriesArgs.PrevLogIndex{%d}+len(appendEntriesArgs.Entries){%d} != lastLogIndex{%d}",
                     		 appendEntriesArgs->prevlogindex(), appendEntriesArgs->entries_size(), lastLogIndex));


			// 构造 RPC 响应
			const std::shared_ptr<raftRpcProctoc::AppendEntriesReply> appendEntriesReply = std::make_shared<raftRpcProctoc::AppendEntriesReply>();
			appendEntriesReply->set_appstate(Disconnected); // 初始设为未连接
			

			// 创建新线程异步发送 AppendEntries RPC -- 正式发送
			std::thread t(&Raft::sendAppendEntries, this, i, appendEntriesArgs, appendEntriesReply, appendNums);
			t.detach();
			
		}

		// 本轮心跳已完成，刷新心跳时间点
		m_lastResetHeartBeatTime = now(); 
	}

}


// leader 向指定节点发送追加日志 AppendEntries RPC，并处理响应
bool Raft::sendAppendEntries(int server,											// 目标follower的编号
					   std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> args,		// 本次 AppendEntries RPC 的参数
					   std::shared_ptr<raftRpcProctoc::AppendEntriesReply> reply,	// Follower 对请求的响应
			 		   std::shared_ptr<int> appendNums) 							// 成功响应心跳的节点个数
{
  	/* 通信ok => 响应正常 => term相同 => 是Leader=> 日志匹配 => 多数成功响应 => 发送日志都是当前term => 提交 */

	DPrintf("[func-Raft::sendAppendEntries-raft{%d}] leader 向节点{%d}发送AE rpc开始 ， args->entries_size():{%d}",
			 m_me, server, args->entries_size());
	
	
	// 发送 RPC 请求并判断网络是否通畅
	bool ok = m_peers[server]->AppendEntries(args.get(), reply.get()); // ok只表示RPC通信是否成功
	if (!ok)
	{
		DPrintf("[func-Raft::sendAppendEntries-raft{%d}] leader 向节点{%d}发送AE rpc失败", m_me, server);
		return ok; // 如果网络断开，不会进入后续的逻辑，直接退出
	}
	DPrintf("[func-Raft::sendAppendEntries-raft{%d}] leader 向节点{%d}发送AE rpc成功", m_me, server);


	// 响应状态判断：若对方返回了“断开连接”的状态，直接跳过处理（Follower 未启动或挂掉）
	if (reply->appstate() == Disconnected) 
	{
		return ok;
	}


	// ----------------------------------------- 处理 reply -------------------------------------------------
	std::lock_guard<std::mutex> lg1(m_mtx);

	// 检查 follower 返回的 term
	if (reply->term() > m_currentTerm) 		// 自己的 term 落后，转为 Follower
	{
		m_status = Follower;
		m_currentTerm = reply->term();
		m_votedFor = -1;
		return ok;
	}
	else if (reply->term() < m_currentTerm) // 对方 term 落后，忽略
	{
		DPrintf("[func -sendAppendEntries  rf{%d}]  节点：{%d}的term{%d}<rf{%d}的term{%d}\n", 
				 m_me, server, reply->term(), m_me, m_currentTerm);
		return ok;
	}


	// 如果当前节点不是Leader，不处理，直接返回
	if (m_status != Leader) 
	{
		return ok;
	}


	// term 相等
	myAssert(reply->term() == m_currentTerm,
			 format("reply.Term{%d} != rf.currentTerm{%d}   ", reply->term(), m_currentTerm));
	
	
	// 判断日志匹配是否失败
	if (!reply->success()) // 日志追加失败，Follower 拒绝日志追加
	{
		// 原因一般是：Leader 提供的 prevLogIndex/prevLogTerm 与 Follower 本地日志不一致，无法通过一致性校验

		// reply->updatenextindex() 表示 Follower 主动返回的建议 Leader 下次应从哪个日志 index 开始发
		if (reply->updatenextindex() != -100) // -100只是一个特殊标记，没有具体含义
		{
			DPrintf("[func -sendAppendEntries  rf{%d}]  返回的日志term相等，但是不匹配，回缩nextIndex[%d]：{%d}\n", 
					m_me, server, reply->updatenextindex());

			m_nextIndex[server] = reply->updatenextindex(); 
		}
	}
	else // 日志匹配成功，follower同意接收本次心跳或者日志 -----------------------------------------------------
	{
		*appendNums = *appendNums + 1; // 成功响应节点数 + 1
		DPrintf("---------------------------tmp------------------------- 节点{%d}返回true,当前*appendNums{%d}", 
				server, *appendNums);

		// 更新该 Follower 的 matchIndex（匹配到 Leader 的最大 index）和 nextIndex（下次要发送的 index）
		m_matchIndex[server] = std::max(m_matchIndex[server], args->prevlogindex() + args->entries_size());
		m_nextIndex[server] = m_matchIndex[server] + 1;
		

		// 防止 nextIndex 越界：保证 nextIndex 不超过当前最后一条日志的下一个位置
		int lastLogIndex = getLastLogIndex();
		myAssert(m_nextIndex[server] <= lastLogIndex + 1,
					format("error msg:rf.nextIndex[%d] > lastLogIndex+1, len(rf.logs) = %d   lastLogIndex{%d} = %d", 
						server, m_logs.size(), server, lastLogIndex));

					
		// 多数节点成功响应 --> 考虑提交日志 --------------------------------------------------------------------
		if (*appendNums >= 1 + m_peers.size() / 2)
		{
			*appendNums = 0;
			if (args->entries_size() > 0)
			{
				DPrintf("args->entries(args->entries_size()-1).logterm(){%d}   m_currentTerm{%d}",
						args->entries(args->entries_size() - 1).logterm(), m_currentTerm);
			}
						
			/*
				领导人完备性（Leader Completeness）：
				"Leader 只能提交当前 term 的日志。只有当这些日志被提交，才会一并视作之前 term 的日志也已提交。"
			*/

			// 必须是当前 term 的日志才能提交 -------------------------------------------------------------------
			if (args->entries_size() > 0 && 
				args->entries(args->entries_size() - 1).logterm() == m_currentTerm)// 检查最后一条被发送日志是否属于当前term
			{
				DPrintf("---------------------------tmp------------------------- 当前term有log成功提交，更新leader的m_commitIndex "
						"from{%d} to{%d}",
						m_commitIndex, args->prevlogindex() + args->entries_size());
				
				// 更新 m_commitIndex
				m_commitIndex = std::max(m_commitIndex, args->prevlogindex() + args->entries_size());
			}

			myAssert(m_commitIndex < lastLogIndex, 
					format("[func-sendAppendEntries,rf{%d}] lastLogIndex:%d  rf.commitIndex:%d\n", m_me, lastLogIndex, m_commitIndex));	
		}
	}


	return ok;

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
	/* Leader 想发送给 Follower 的日志条目中，新日志的前一条日志的索引和term*/

	// 假设：
	// Leader日志为索引：[1, 2, 3, 4, 5]
	// Follower日志为索引：[1, 2, 3]
	// 当Leader想追加索引4和5的日志时，leader就会自己确定preLogIndex = 3
	// 然后RPC发送给follower告诉Follower，follower去判断这些信息是否与自己的日志一致

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






void Raft::leaderSendSnapShot(int server) 
{
  
   
}


void Raft::leaderUpdateCommitIndex() 
{
 
}


// 进来前要保证logIndex是存在的，即≥rf.lastSnapshotIncludeIndex	，而且小于等于rf.getLastLogIndex()
bool Raft::matchLog(int logIndex, int logTerm) 
{
 
}


void Raft::persist() 
{
  
}


// RPC接口重写，用于接收其他节点发来的 “投票请求”
void Raft::RequestVote(google::protobuf::RpcController* controller, const ::raftRpcProctoc::RequestVoteArgs* request,
                       ::raftRpcProctoc::RequestVoteReply* response, ::google::protobuf::Closure* done) 
{
  RequestVote(request, response);
  done->Run();
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



// 获取当前日志数组中最后一条日志的任期号
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