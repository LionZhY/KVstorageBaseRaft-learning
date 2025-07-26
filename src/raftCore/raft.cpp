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
#include <vector>
#include "ApplyMsg.h"
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


//////////////////////////////////////////////  初始化节点  //////////////////////////////////////////////

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





//////////////////////////////////////////////  leader 选举  //////////////////////////////////////////////


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



// 发起选举  （Candidate）
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


// 判断候选人日志是否不落后于自己（用于投票）
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

			myAssert(m_commitIndex <= lastLogIndex, 
					format("[func-sendAppendEntries,rf{%d}] lastLogIndex:%d  rf.commitIndex:%d\n", m_me, lastLogIndex, m_commitIndex));	
		}
	}


	return ok;
}



// follower 处理 leader 发来的日志追加请求，包括心跳（实际处理 AppendEntries 的内部实现）
void Raft::AppendEntries1(const raftRpcProctoc::AppendEntriesArgs* args, 
						  raftRpcProctoc::AppendEntriesReply* reply) 
{
	std::lock_guard<std::mutex> locker(m_mtx);

	reply->set_appstate(AppNormal); // 设置 AppState 告诉 Leader 网络连接是正常的

	// leader 的 term 落后
	if (args->term() < m_currentTerm)
	{
		reply->set_success(false);			// 拒绝追加
		reply->set_term(m_currentTerm); 	// 返回当前自己的term
		reply->set_updatenextindex(-100); 	// 提示 Leader 应回退日志
		DPrintf("[func-AppendEntries-rf{%d}] 拒绝了 因为Leader{%d}的term{%v}< rf{%d}.term{%d}\n", 
				m_me, args->leaderid(), args->term(), m_me, m_currentTerm);
		
		return; // 注意从过期的Leader那收到消息，不需要重设定时器
	}

	// 延迟执行持久化
	DEFER { persist(); };// 由于DEFER创建在锁之后，因此执行persist的时候，锁仍然处于持有状态，确保线程安全
	
	// leader 的 term 更新
	if (args->term() > m_currentTerm)
	{
		// term合理，三变：身份，term，投票
		m_status = Follower;			// 退回follower(当前有可能是Candidate)
		m_currentTerm = args->term();	// 更新自己的term
		m_votedFor = -1;				// 重置 votedFor，可以重新投票

		// 这里不返回，让该节点尝试接收日志
	}


	// term 相等 ---------------------------------------------------------------------------------------------
	myAssert(args->term() == m_currentTerm, format("assert {args.Term == rf.currentTerm} fail"));
	
	m_status = Follower;			 // Leader/Follower/Candidate 收到当前 term 的 Leader 心跳，都必须退为 Follower						 
	m_lastResetElectionTime = now(); // 重置选举定时器



	// 日志一致性检查
	if (args->prevlogindex() > getLastLogIndex()) 
	{
		// Leader 要追加的 prevLogIndex 太新，和follower不衔接
		reply->set_success(false);
		reply->set_term(m_currentTerm);
		reply->set_updatenextindex(getLastLogTerm() + 1); // 提示 Leader 应从哪里开始发
		return;
	}
	else if (args->prevlogindex() < m_lastSnapshotIncludeIndex) 
	{
		// Leader 的 prevLogIndex 太旧，已被快照收录走了，过时日志，已经不在m_logs中
		reply->set_success(false);
		reply->set_term(m_currentTerm);
		reply->set_updatenextindex(m_lastSnapshotIncludeTerm + 1); 
	}
	

	// Leader 的 prevLogIndex合适，判断日志匹配，尝试复制日志 -------------------------------------------------------
	if (matchLog(args->prevlogindex(), args->prevlogterm()))
	{
		// 遍历每条要追加的日志
		for (int i = 0; i < args->entries_size(); i++)
		{
			auto log = args->entries(i);
			if (log.logindex() > getLastLogIndex()) // 要添加的logindex超过自己的lastLogIndex，新日志，直接添加
			{
				m_logs.push_back(log);
			}
			else // 本地已存在该条目或同位置的日志 (可能因为网络重传或日志覆盖，Leader发送的日志有一部分是本地已有的旧日志)
			{
				// 需要逐条比对该日志条目是否与本地相同: term 和 command

				// term 和 command 都相同，说明该日志已经存在，不用修改

				// term 相同但 command 不同，说明日志冲突，属于协议异常，断言失败
				if (m_logs[getSlicesIndexFromLogIndex(log.logindex())].logterm() == log.logterm() && 
					m_logs[getSlicesIndexFromLogIndex(log.logindex())].command() != log.command())
				{
					myAssert(false, format("[func-AppendEntries-rf{%d}] 两节点logIndex{%d}和term{%d}相同，但是其command{%d:%d}   "
                                 		   " {%d:%d}却不同！！\n",
										   m_me, log.logindex(), log.logterm(), m_me, m_logs[getSlicesIndexFromLogIndex(log.logindex())].command(), 
										   args->leaderid(), log.command()));
				}

				// 如果 term 不同，说明日志冲突，要用新日志替换本地旧日志
				if (m_logs[getSlicesIndexFromLogIndex(log.logindex())].logterm() != log.logterm())
				{
					m_logs[getSlicesIndexFromLogIndex(log.logindex())] = log;
				}
			}
		}


		// 追加日志完成，断言确认日志末尾至少覆盖了追加的所有日志条目
		myAssert(getLastLogIndex() >= args->prevlogindex() + args->entries_size(), 
				 format("[func-AppendEntries1-rf{%d}]rf.getLastLogIndex(){%d} != args.PrevLogIndex{%d}+len(args.Entries){%d}",
    	           		m_me, getLastLogIndex(), args->prevlogindex(), args->entries_size()));		

		// 更新 commitIndex  
		if (args->leadercommit() > m_commitIndex)
		{
			// 如果 Leader 的 commitIndex 比 Follower 本地的更大，Follower 跟进提交，但是不能超过自己的日志范围
			// 但是也可能存在args->leadercommit() 落后于 getLastLogIndex()的情况，所以取 min()
			m_commitIndex = std::min(args->leadercommit(), getLastLogIndex());	
		}


		// 断言 Follower 的 commitIndex 不应该超过自己当前拥有的日志条目
		myAssert(getLastLogIndex() >= m_commitIndex, 
				 format("[func-AppendEntries1-rf{%d}]  rf.getLastLogIndex{%d} < rf.commitIndex{%d}", 
						m_me, getLastLogIndex(), m_commitIndex));


		// 返回成功 -----------------------------------------------------------------------------------------
		reply->set_success(true);
		reply->set_term(m_currentTerm);
		return;

	}
	else // 日志不匹配  !matchLog(args->prevlogindex(), args->prevlogterm())
	{
		// 这是 Raft 论文中日志一致性检查失败的情况：
		// args->prevlogindex() 在 Follower 本地存在；但 args->prevlogterm() 与本地该 index 的 term 不一致

		/*
			如果不优化，原始论文中 Leader 就会退一格重发，直到找到一个匹配点。很慢

			优化： 
    		PrevLogIndex 长度合适，但是不匹配，因此往前寻找 矛盾的term的第一个元素，然后从这个位置往后都重新发
    		该term的日志也不一定都是矛盾的，只是这么优化减少rpc而已
    		什么时候term会矛盾呢？很多情况，比如leader接收了日志之后马上就崩溃等等
		*/

		// 保守处理：告诉 Leader 退一步，下次从 prevLogIndex 发
		reply->set_updatenextindex(args->prevlogindex());

		// 优化：follower 直接建议 Leader 出现矛盾的整段 term 都重新发
		for (int index = args->prevlogindex(); index >= m_lastSnapshotIncludeIndex; --index)
		{
			// Follower 从 prevlogindex() 开始往前回退，退到和现在这个本地term不一样的位置（其实就是上一个term的边界）
			if (getLogTermFromLogIndex(index) != getLogTermFromLogIndex(args->prevlogindex()))
			{
				reply->set_updatenextindex(index + 1); // 建议Leader从这个位置重新发
				break;
				// 相当于是跳过了现在出现不匹配的这个term的日志，告诉Leader这个term都重新发
				// 比起一格一格试，Leader 能一次跳回一整段有冲突的 term
			}
		}

		reply->set_success(false);
		reply->set_term(m_currentTerm);
		return;
	}

}


// leader调用 获取想发给 Follower 的新日志的上一条日志的信息：prevLogIndex 和 prevLogTerm
void Raft::getPrevLogInfo(int server, int* preIndex, int* preTerm) // 传入：服务器index，传出：AE请求的preLogIndex和PrevLogTerm
{
	/* Leader 想发送给 Follower 的日志条目中，新日志的前一条日志的索引和term */

	// 假设：
	// 	Leader日志为索引：  [1, 2, 3, 4, 5]
	// 	Follower日志为索引：[1, 2, 3]
	// 	当Leader想追加索引4和5的日志时，leader就会自己确定 preLogIndex = 3
	// 	然后RPC发送给follower，follower去判断这些信息是否与自己的日志一致

	// Leader下一个要发送的日志正好是 lastSnapshotIncludeIndex + 1，说明 Leader 只能从快照后第一条日志开始发起同步
	if (m_nextIndex[server] == m_lastSnapshotIncludeIndex + 1)
	{
		// 要发送的日志是第一个日志，因此直接返回 m_lastSnapshotIncludeIndex 和 m_lastSnapshotIncludeTerm
		*preIndex = m_lastSnapshotIncludeIndex;
		*preTerm = m_lastSnapshotIncludeTerm;
		return;
		// Leader 告诉 Follower：“我认为你上一次收到的最后一条日志是快照最后一项”
	}

	// 否则从日志中提取
	auto nextIndex = m_nextIndex[server];
	*preIndex = nextIndex - 1; // 从本地日志中获取 nextIndex - 1 对应的日志项作为 “prevLog”
	*preTerm = m_logs[getSlicesIndexFromLogIndex(*preIndex)].logterm();

}



// 判断本地日志项 LogIndex 的 term 和 Leader 发来的是否匹配
bool Raft::matchLog(int logIndex, int logTerm) 
{
	/* 某个日志项 logIndex 在当前 Follower 节点是否存在，并且 term 是否与 Leader 发来的匹配 */
	
	myAssert(logIndex >= m_lastSnapshotIncludeIndex && logIndex <= getLastLogIndex(), // 检查 logIndex 是否落在合法范围内
			format("不满足：logIndex{%d}>=rf.lastSnapshotIncludeIndex{%d}&&logIndex{%d}<=rf.getLastLogIndex{%d}",
                  logIndex, m_lastSnapshotIncludeIndex, logIndex, getLastLogIndex()));
	
	return logTerm == getLogTermFromLogIndex(logIndex); 
}


// leader 根据多数节点复制日志进度，更新 CommitIndex (好像没用上)
void Raft::leaderUpdateCommitIndex() 
{
	/* Leader 根据每个 Follower 的 matchIndex[]，决定当前有哪些日志已经被「多数派节点」复制成功 */
	/* Raft 只能提交本 term 由 Leader 新生成且多数派复制成功的日志条目 */

	m_commitIndex = m_lastSnapshotIncludeIndex; // 重置 commitIndex 为快照最后一条日志的 index，作为最低基线

	// 从最新日志项往前查，尝试找到一个满足 “多数派复制成功” 的日志 index
	for (int index = getLastLogIndex(); index >= m_lastSnapshotIncludeIndex + 1; index--)
	{
		int sum = 0;
		for (int i = 0; i < m_peers.size(); i++)
		{
			if (i == m_me) // Leader 本身也算一个确认
			{
				sum += 1;
				continue;
			}
			if (m_matchIndex[i] >= index) 
			{
				sum += i; // 有几个 Follower 的日志 index >= 当前 index
			}
		}

		
		// sum >= n/2 + 1 即为多数派，且日志的任期等于当前任期，才会更新commitIndex！！！！(只能提交当前 term 的日志)
		if (sum >= m_peers.size() / 2 + 1 && getLogTermFromLogIndex(index) == m_currentTerm)
		{
			m_commitIndex = index;
			break;
		}

	}
}



///////////////////////////////////////////  leader 接收客户端命令  //////////////////////////////////////////

// Leader 节点接收上层（KVServer）客户端命令，生成日志项
void Raft::Start(Op command, 						// 客户端提交的命令
				 int* newLogIndex, int* newLogTerm, // 新日志的index和term
				 bool* isLeader) 					// 返回是否是Leader（非Leader不能接收命令）
{
	/* 	
		当上层（如 KVServer）调用 Start(command) 向 Raft 提交一个新命令时，
		Leader 将该命令封装为日志项，追加到本地日志 m_logs 中，并返回该日志项的索引和任期。
	*/

	std::lock_guard<std::mutex> lg(m_mtx);

	// 非 Leader 拒绝接收命令
	if (m_status != Leader)
	{
		DPrintf("[func-Start-rf{%d}]  is not leader");
		*newLogIndex = -1;
		*newLogTerm = -1;
		*isLeader = false;
		return;
	}

	// 构造新日志项LogEntry 并追加到日志 m_logs
	raftRpcProctoc::LogEntry newLogEntry;
	newLogEntry.set_command(command.asString()); 	// 序列化命令内容
	newLogEntry.set_logterm(m_currentTerm);			// 当前任期
	newLogEntry.set_logindex(getNewCommandIndex());	// 新日志索引
	m_logs.emplace_back(newLogEntry);				// 添加到日志数组 m_logs

	//  获取最新日志索引，打印日志
	int lastLogIndex = getLastLogIndex();
	DPrintf("[func-Start-rf{%d}]  lastLogIndex:%d,command:%s\n", m_me, lastLogIndex, &command);

	// 持久化日志状态
	persist();

	// 设置返回值，表示命令成功提交
	*newLogIndex = newLogEntry.logindex();
	*newLogTerm = newLogEntry.logterm();
	*isLeader = true;

	
	/* 延迟同步，Leader 不会因新命令立即向 Follower 发送日志，而是等下一次定时心跳统一触发 AppendEntries */
}





////////////////////////////////////////  ApplyMsg 推送到 KVServer  ////////////////////////////////////////

// 提取已提交但未应用的日志封装成 ApplyMsg
std::vector<ApplyMsg> Raft::getApplyLogs() 
{
	/* 从日志中提取 尚未应用到状态机 的日志条目，封装成 ApplyMsg 列表，供上层状态机（如 KV Server）处理 */
	/* ApplyMsg 是用于 Raft 向状态机（KV 层）提交应用的消息结构 */

	std::vector<ApplyMsg> applyMsgs;

	// 断言检查：当前 commitIndex 不应该超过日志的实际最大 index
	myAssert(m_commitIndex <= getLastLogIndex(), 
			 format("[func-getApplyLogs-rf{%d}] commitIndex{%d} >getLastLogIndex{%d}",
                    m_me, m_commitIndex, getLastLogIndex()));
	
	// 循环处理 未应用的日志
	while (m_lastApplied < m_commitIndex)
	{
		m_lastApplied++; // 每次向前推进一个
		
		// 断言确保 实际获取到的日志条目的 logindex 应该与当前的 m_lastApplied 一致
		myAssert(m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].logindex() == m_lastApplied,
             	 format("rf.logs[rf.getSlicesIndexFromLogIndex(rf.lastApplied)].LogIndex{%d} != rf.lastApplied{%d} ",
                    	m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].logindex(), m_lastApplied));

		// 为当前 m_lastApplied 日志构造一条 ApplyMsg
		ApplyMsg applyMsg;
		applyMsg.CommandValid = true;  // 表示这是一个正常日志命令，而不是快照
		applyMsg.SnapshotValid = false;
		applyMsg.Command = m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].command(); // 提取具体的日志命令
		applyMsg.CommandIndex = m_lastApplied; // 该命令在 Raft 日志中的逻辑索引
		
		// 把构造好的消息加入 applyMsgs 列表，准备返回给上层状态机
		applyMsgs.emplace_back(applyMsg);

	}

	// 返回这批待应用的日志命令，推送给 KV Server 执行
	return applyMsgs;
}


// 周期性将已提交的日志推入 applyChan 通道 （正常日志命令）
void Raft::applierTicker() 
{
	/* 周期性将已提交的日志应用到状态机（KVServer）的后台线程逻辑核心 */
	/* 它保证状态机和 commitIndex 保持一致性，即 Raft 所提交的日志最终会被应用 */
	/* 由独立线程或协程在 Raft 节点启动时启动 */

	while (true)
	{
		m_mtx.lock();

		// 当前节点是 Leader 时打印调试：当前已应用日志（m_lastApplied）与已提交日志（m_commitIndex）的差距
		if (m_status == Leader)
		{
			DPrintf("[Raft::applierTicker() - raft{%d}]  m_lastApplied{%d}   m_commitIndex{%d}", 
					m_me, m_lastApplied, m_commitIndex);
		}

		// 获取尚未应用但已提交的日志，封装成 ApplyMsg 列表
		auto applyMsgs = getApplyLogs();

		m_mtx.unlock(); // 后续将日志推送给 KVServer 时，不需要锁，避免阻塞主线程

		
		if (!applyMsgs.empty()) 
		{
			DPrintf("[func- Raft::applierTicker()-raft{%d}] 向kvserver报告的applyMsgs长度为：{%d}", 
					m_me, applyMsgs.size());
		}
		
		// 将所有待应用的 ApplyMsg 推入 applyChan 通道，上层状态机（KVServer）会监听该Channel，取出并执行 command
		for (auto& message : applyMsgs)
		{
			applyChan->Push(message);
		}


		// 每轮应用检查结束后，等待一小段时间，防止 CPU 忙等
		sleepNMilliseconds(ApplyInterval); // ApplyInterval - Apply 状态机的检查间隔
	}
  
}


// 将应用消息推送给KV服务层（快照）
void Raft::pushMsgToKvServer(ApplyMsg msg) 
{ 
	/* Raft 向状态机（KVServer）推送应用消息（ApplyMsg） 的封装，是 Raft 与 KV 服务层之间通信的接口 */

	/* 但是只在 InstallSnapshot() 处理逻辑中调用，所以主要用来将快照 ApplyMsg 推送给状态机 */
	
	applyChan->Push(msg);
}





////////////////////////////////////////////  日志信息辅助获取  ////////////////////////////////////////////

/*
	逻辑索引（logIndex）：Raft 日志全局编号，不变，即使前面被快照清除。
	物理索引（slice index）：实际在 m_logs 数组中的下标，从 0 开始。
*/

// 获取当前节点最后一条日志项的逻辑索引和任期
void Raft::getLastLogIndexAndTerm(int* lastLogIndex, int* lastLogTerm) 
{
	// 若日志为空，表示当前日志都被快照清理了，只能返回快照中最后一项
	if (m_logs.empty())
	{
		*lastLogIndex = m_lastSnapshotIncludeIndex;
		*lastLogTerm = m_lastSnapshotIncludeTerm;
		return;
	}
	else // 从本地日志中获取最新一条日志条目的索引 (logindex) 和任期 (logterm)
	{
		*lastLogIndex = m_logs[m_logs.size() - 1].logindex();
		*lastLogTerm = m_logs[m_logs.size() - 1].logterm();
		return;
	}
}

// 获取当前节点最后一条日志条目的逻辑索引（LogIndex）
int Raft::getLastLogIndex() 
{
	int lastLogIndex = -1;
	int _ = -1; // 占位变量（约定俗成的“哑变量名”）
	getLastLogIndexAndTerm(&lastLogIndex, &_);
	return lastLogIndex;
}

// 获取当前日志数组中最后一条日志的 term
int Raft::getLastLogTerm() 
{
	int _ = -1;
	int lastLogTerm = -1;
	getLastLogIndexAndTerm(&_, &lastLogTerm);
	return lastLogTerm;
}



// 获取指定 logIndex 的对应 term  
int Raft::getLogTermFromLogIndex(int logIndex) 
{
	/* logIndex 是逻辑索引，不是m_logs数组下标，因为中间可能存在快照截断 */

	// 要求 logIndex 必须在 [m_lastSnapshotIncludeIndex, getLastLogIndex()] 之间
	myAssert(logIndex >= m_lastSnapshotIncludeIndex,
             format("[func-getSlicesIndexFromLogIndex-rf{%d}]  index{%d} < rf.lastSnapshotIncludeIndex{%d}", 
					m_me, logIndex, m_lastSnapshotIncludeIndex));
	
	int lastLogIndex = getLastLogIndex();
	myAssert(logIndex <= lastLogIndex, 
			 format("[func-getSlicesIndexFromLogIndex-rf{%d}]  logIndex{%d} > lastLogIndex{%d}",
                    m_me, logIndex, lastLogIndex));


	// 如果请求的是快照中最后一条的 term
	if (logIndex == m_lastSnapshotIncludeIndex)
	{
		return m_lastSnapshotIncludeTerm;
	}
	else  // 否则查找日志数组中对应的日志 term
	{
		return m_logs[getSlicesIndexFromLogIndex(logIndex)].logterm();
	}	
}



// 找到逻辑索引 logIndex 对应的物理索引 SliceIndex
int Raft::getSlicesIndexFromLogIndex(int logIndex) 
{
	/* 将某条日志的 “逻辑索引（logIndex）” 转换为 m_logs 容器中的真实下标（物理索引） */

	// 要求 logIndex 必须在 (m_lastSnapshotIncludeIndex, getLastLogIndex()] 之间 （不包含snapshot）
	myAssert(logIndex > m_lastSnapshotIncludeIndex,
	 		  format("[func-getSlicesIndexFromLogIndex-rf{%d}]  index{%d} <= rf.lastSnapshotIncludeIndex{%d}",
					 m_me, logIndex, m_lastSnapshotIncludeIndex));
	int lastLogIndex = getLastLogIndex();
	myAssert(logIndex <= lastLogIndex, 
			 format("[func-getSlicesIndexFromLogIndex-rf{%d}]  logIndex{%d} > lastLogIndex{%d}",
                    m_me, logIndex, lastLogIndex));

	// m_logs 容器中的下标
	int SliceIndex = logIndex - m_lastSnapshotIncludeIndex - 1;
	return SliceIndex;
}




// 用于外部模块获取当前 Raft 节点的两个状态：term，是否是Leader
void Raft::GetState(int* term, bool* isLeader) 
{
	m_mtx.lock();
	DEFER { m_mtx.unlock();	};

	*term = m_currentTerm;
	*isLeader = (m_status == Leader);
}



// 获取一个新客户端命令 应该分配的逻辑日志索引（LogIndex）
int Raft::getNewCommandIndex() 
{
	// 如果len(logs)==0,就为快照的index+1，否则为log最后一个日志+1
	auto lastLogIndex = getLastLogIndex();
	return lastLogIndex + 1;
}






//////////////////////////////////////////////  快照相关  //////////////////////////////////////////////

// 在当前服务器上 生成快照并截断旧日志
void Raft::Snapshot(int index, std::string snapshot) 
{
	/* 	Snapshot 没有主动 “生成快照” 的操作，只是：
		更新了快照元信息（m_lastSnapshotIncludeIndex 和 m_lastSnapshotIncludeTerm）；
		截断了旧日志;
		把 snapshot 写入了持久化层
	*/
	
	/* 	
		快照内容不是 Raft 层生成的，而是上层状态机主动传入的
	   	上层状态机（如 KVServer）在合适的 applyIndex 时机，序列化当前状态（如 KV map），生成 snapshot string
		Raft::Snapshot() 接收 snapshot --> 截断日志，更新快照最后日志信息，持久化snapshot
	*/

	std::lock_guard<std::mutex> lg(m_mtx);
	
	if (m_lastSnapshotIncludeIndex >= index ||  // 当前已有的快照索引超过新快照索引（重复或倒退）
		index > m_commitIndex) // 或请求快照的 index 超过当前 commitIndex（即尚未提交的日志不能被截断）
	{
		// 拒绝操作，直接返回
		DPrintf(
        	"[func-Snapshot-rf{%d}] rejects replacing log with snapshotIndex %d as current snapshotIndex %d is larger or smaller ",
        	m_me, index, m_lastSnapshotIncludeIndex);
		return;
	}
	
	auto lastLogIndex = getLastLogIndex(); // 为了检查snapshot前后日志是否一样，防止多截取或者少截取日志

	int newLastSnapshotIncludeIndex = index; // 新快照的最后 index 和 term
	int newLastSnapshotIncludeTerm = m_logs[getSlicesIndexFromLogIndex(index)].logterm();


	// 构造保留的日志列表（index+1 到末尾，收集所有未被快照包含的日志条目）
	std::vector<raftRpcProctoc::LogEntry> trunckedLogs;
	for (int i = index + 1; i <= getLastLogIndex(); i++) // 包括等号，确保保留最后一个日志
	{
		trunckedLogs.push_back(m_logs[getSlicesIndexFromLogIndex(i)]);
	}


	// 直接用保留日志覆盖 m_logs，更新快照最后日志信息
	m_lastSnapshotIncludeIndex = newLastSnapshotIncludeIndex;
	m_lastSnapshotIncludeTerm = newLastSnapshotIncludeTerm;
	m_logs = trunckedLogs;


	// 快照生成代表 index 之前的数据已被“安全应用”，因此将 commitIndex 和 lastApplied 至少推进到 index
	m_commitIndex = std::max(m_commitIndex, index);
	m_lastApplied = std::max(m_lastApplied, index);


	// 持久化状态和快照内容
	m_persister->Save(persistData(), snapshot);

	DPrintf("[SnapShot]Server %d snapshot snapshot index {%d}, term {%d}, loglen {%d}", 
			m_me, index, m_lastSnapshotIncludeTerm, m_logs.size());
  	
	// 断言检查 当前日志条目数量 + 快照覆盖条目数量 = 原始日志数量
	myAssert(m_logs.size() + m_lastSnapshotIncludeIndex == lastLogIndex,
           	 format("len(rf.logs){%d} + rf.lastSnapshotIncludeIndex{%d} != lastLogjInde{%d}", 
					m_logs.size(), m_lastSnapshotIncludeIndex, lastLogIndex));
} 



// 条件安装快照，判断快照是否比当前状态新，决定是否安装
bool Raft::CondInstallSnapshot(int lastIncludedTerm, int lastIncludedIndex, std::string snapshot) 
{
	return true; // ???
}



// leader 向落后follower发送快照
void Raft::leaderSendSnapShot(int server) 
{
	m_mtx.lock();

	// 构造 InstallSnapshotRequest
	raftRpcProctoc::InstallSnapshotRequest args;
	args.set_leaderid(m_me);
	args.set_term(m_currentTerm);
	args.set_lastsnapshotincludeindex(m_lastSnapshotIncludeIndex);
	args.set_lastsnapshotincludeterm(m_lastSnapshotIncludeTerm);
	args.set_data(m_persister->ReadSnapshot()); // 读取持久化的快照数据（完整快照准备发送给follower）

	// 构造 InstallSnapshotResponse
	raftRpcProctoc::InstallSnapshotResponse reply;

	m_mtx.unlock();


	// 发送RPC请求，ok 表示网络是否成功响应
	bool ok = m_peers[server]->InstallSnapshot(&args, &reply); // InstallSnapshot() 是 RPC 方法，阻塞式等待响应
	

	// RPC 返回后重新加锁
	m_mtx.lock();
	DEFER { m_mtx.unlock(); };

	// RPC 失败，直接返回
	if (!ok) { 
	  return;
	}

	// 检查当前节点是否还处于 Leader  (可能刚刚 RPC 过程中发生了选举或状态变化)
	if (m_status != Leader || m_currentTerm != args.term()) 
	{
	  return;  //中间释放过锁，可能状态已经改变了
	}

	// 检查 term
	if (reply.term() > m_currentTerm) 
	{
	  // 自己落后，三变：Term、角色、投票记录
	  m_currentTerm = reply.term();
	  m_votedFor = -1;
	  m_status = Follower;

	  persist();
	  m_lastResetElectionTime = now(); 
	  return;
	}

	// RPC正常返回，则更新该 Follower 的同步进度
	m_matchIndex[server] = args.lastsnapshotincludeindex();
	m_nextIndex[server] = m_matchIndex[server] + 1;
}



// followr 接收 leader 发来的快照请求，同步快照到本机（直接 RPC 调用）
void Raft::InstallSnapshot(const raftRpcProctoc::InstallSnapshotRequest* args,
                           raftRpcProctoc::InstallSnapshotResponse* reply) 
{
	m_mtx.lock();
	DEFER { m_mtx.unlock(); };

	// 检查 term
	if (args->term() < m_currentTerm)
	{
		reply->set_term(m_currentTerm);
		return;
	}

	if (args->term() > m_currentTerm)
	{
		m_currentTerm = args->term();
		m_votedFor = -1;
		m_status = Follower;
		persist();
	}

	// 无论什么身份都确保退回 Follower，重置选举超时计时器
	m_status = Follower;
	m_lastResetElectionTime = now();


	// 检查 args->snapshot 是否过时 (比自己已有的还旧)
	if (args->lastsnapshotincludeindex() <= m_lastSnapshotIncludeIndex)
	{
		return;
	}


	// term相同，args->snapshot不过时 ==> 根据 args->lastsnapshotincludeindex() 处理 m_logs
	auto lastLogIndex = getLastLogIndex();

	if (lastLogIndex > args->lastsnapshotincludeindex())
	{
		// lastSnapshotIncludeIndex < args->lastsnapshot < lastLogIndex 部分日志过期，截断
		m_logs.erase(m_logs.begin(), m_logs.begin() + getSlicesIndexFromLogIndex(args->lastsnapshotincludeindex()) + 1);
	}
	else 
	{
		// lastLogIndex < args->lastsnapshot 日志全过期，清空
		m_logs.clear();
	}

	// 更新本地状态，这些字段的更新代表了本地已经应用了该快照
	m_commitIndex = std::max(m_commitIndex, args->lastsnapshotincludeindex());
	m_lastApplied = std::max(m_lastApplied, args->lastsnapshotincludeindex());
	m_lastSnapshotIncludeIndex = args->lastsnapshotincludeindex();
	m_lastSnapshotIncludeTerm = args->lastsnapshotincludeterm();

	// 回复RPC成功
	reply->set_term(m_currentTerm);


	// 异步将快照交给状态机（KV Server）
	ApplyMsg msg; // ApplyMsg 类型的消息结构，用于向上层状态机发送应用信息
	msg.SnapshotValid = true;
	msg.Snapshot = args->data();
	msg.SnapshotTerm = args->lastsnapshotincludeterm();
	msg.SnapshotIndex = args->lastsnapshotincludeindex();

	std::thread t(&Raft::pushMsgToKvServer, this, msg); // 创建新线程并执行 pushMsgToKvServer()
	t.detach();

	// 持久化状态与快照
	m_persister->Save(persistData(), args->data());
}




////////////////////////////////////////////////  持久化  ////////////////////////////////////////////////

// 当前状态持久化 (写入磁盘)
void Raft::persist() 
{
    auto data = persistData(); 		  // 序列化所有应持久化的字段
  	m_persister->SaveRaftState(data); // 序列化后的 Raft 状态数据 写入 m_persister 所管理的持久化存储
}


// 将需要持久化的状态打包为字符串（即序列化）
std::string Raft::persistData() 
{
	// 将状态封装到 BoostPersistRaftNode 中，然后使用 Boost 序列化为字符串
	BoostPersistRaftNode boostPersistRaftNode; 
	boostPersistRaftNode.m_currentTerm = m_currentTerm;
	boostPersistRaftNode.m_votedFor = m_votedFor;
	boostPersistRaftNode.m_lastSnapshotIncludeIndex = m_lastSnapshotIncludeIndex;
	boostPersistRaftNode.m_lastSnapshotIncludeTerm = m_lastSnapshotIncludeTerm;
	for (auto& item : m_logs) 
	{
		// 日志 m_logs 是 Protobuf 类型的，单独用 .SerializeAsString() 序列化成字符串加入
		boostPersistRaftNode.m_logs.push_back(item.SerializeAsString());
	}

	// 最后统一使用 Boost::text_oarchive 序列化为文本格式的字符串，方便后续存储
	std::stringstream ss;
	boost::archive::text_oarchive oa(ss);
	oa << boostPersistRaftNode;
	return ss.str();
}



// 从持久化数据中恢复 Raft 状态（即反序列化）
void Raft::readPersist(std::string data) 
{
    if (data.empty()) 
	{
    	return;
  	}

	// 使用 Boost 的 text_iarchive 对象反序列化数据
  	std::stringstream iss(data);
  	boost::archive::text_iarchive ia(iss);

  	// 还原出当前任期、投票信息、快照元信息
  	BoostPersistRaftNode boostPersistRaftNode;
  	ia >> boostPersistRaftNode;

  	m_currentTerm = boostPersistRaftNode.m_currentTerm;
  	m_votedFor = boostPersistRaftNode.m_votedFor;
  	m_lastSnapshotIncludeIndex = boostPersistRaftNode.m_lastSnapshotIncludeIndex;
  	m_lastSnapshotIncludeTerm = boostPersistRaftNode.m_lastSnapshotIncludeTerm;
  	m_logs.clear();
  	for (auto& item : boostPersistRaftNode.m_logs)  
	{
  	  	raftRpcProctoc::LogEntry logEntry;
  	  	logEntry.ParseFromString(item);// 对每条日志项再使用 Protobuf 的 ParseFromString() 恢复日志结构
  	  	m_logs.emplace_back(logEntry);
  	}
}


// 返回当前持久化状态的占用空间大小
int Raft::GetRaftStateSize() 
{ 
	return m_persister->RaftStateSize(); 
}





//////////////////////////////////////////////  RPC 接口重写  //////////////////////////////////////////////

/* 
	Raft 节点对外提供的 gRPC 服务接口 
	是 Raft 节点作为 RPC 服务端 时处理 RPC 请求的入口函数
	对应的 AppendEntries1(), InstallSnapshot(), RequestVote() 是核心的业务逻辑函数；
	
	done 是 protobuf/gRPC 框架传入的完成回调函数（Closure），
	调用 done->Run() 表示通知框架 “本次 RPC 请求处理结束，可以将 response 返回给客户端”
*/


// RPC接口重写，接收远程追加日志请求
void Raft::AppendEntries(google::protobuf::RpcController* controller, // RPC 框架控制器
                         const ::raftRpcProctoc::AppendEntriesArgs* request,
                         ::raftRpcProctoc::AppendEntriesReply* response, 
						 ::google::protobuf::Closure* done) // 表示“处理完了”的回调
{
	// 序列化，反序列化等操作rpc框架都已经做完了，因此这里只需要获取值然后真正调用本地方法即可
	AppendEntries1(request, response); 
	done->Run();
}

// RPC接口重写，用于接收其他节点发来的 “投票请求”
void Raft::RequestVote(google::protobuf::RpcController* controller, 
					   const ::raftRpcProctoc::RequestVoteArgs* request,
					   ::raftRpcProctoc::RequestVoteReply* response, 
					   ::google::protobuf::Closure* done) 
{
	RequestVote(request, response);
	done->Run();
}


// RPC接口重写，接收远程快照安装请求
void Raft::InstallSnapshot(google::protobuf::RpcController* controller,
                           const ::raftRpcProctoc::InstallSnapshotRequest* request,
                           ::raftRpcProctoc::InstallSnapshotResponse* response, 
						   ::google::protobuf::Closure* done) 
{
	InstallSnapshot(request, response);
	done->Run();
}







