#include "include/Persister.h"
#include "util.h"


// 构造：初始化文件名、清空旧文件、绑定输出流
Persister::Persister(const int me) // me 是节点编号，根据节点编号me为生成的两个文件命名（在当前工作目录下）
    : m_raftStateFileName("raftstatePersist" + std::to_string(me) + ".txt"),// raftstatePersist<me>.txt 保存raft状态
      m_snapshotFileName("snapshotPersist" + std::to_string(me) + ".txt"),// snapshotPersist<me>.txt 保存快照
      m_raftStateSize(0)  	  
{
	// 打开文件并清空旧内容，确保是干净的初始状态
	bool fileOpenFlag = true;
	std::fstream file(m_raftStateFileName, std::ios::out | std::ios::trunc);
	if (file.is_open()) 
	{
		file.close();
	} 
	else 
	{
		fileOpenFlag = false;
	}

	file = std::fstream(m_snapshotFileName, std::ios::out | std::ios::trunc);
	if (file.is_open()) 
	{
		file.close();
	} 
	else 
	{
		fileOpenFlag = false;
	}

	if (!fileOpenFlag) 
	{
		DPrintf("[func-Persister::Persister] file open error");
	}

	// 打开文件绑定写入流，准备后续写入
	m_raftStateOutStream.open(m_raftStateFileName);
	m_snapshotOutStream.open(m_snapshotFileName);
}


// 析构：关闭写入流，释放资源
Persister::~Persister() 
{
	if (m_raftStateOutStream.is_open()) 
	{
		m_raftStateOutStream.close();
	}
	if (m_snapshotOutStream.is_open()) 
	{
		m_snapshotOutStream.close();
	}
}




// 一次性保存raft状态和快照
void Persister::Save(const std::string raftstate, const std::string snapshot) 
{
	std::lock_guard<std::mutex> lg(m_mtx); // 锁保护防止并发写
	clearRaftStateAndSnapshot(); // 先清空旧状态

	// 将raftstate和snapshot写入本地文件
	m_raftStateOutStream << raftstate;
	m_snapshotOutStream << snapshot;
}


// 保存单独raft状态
void Persister::SaveRaftState(const std::string &data) // 专门用于追加日志或更新 term 时的状态写入
{
	std::lock_guard<std::mutex> lg(m_mtx);
	// 将raftstate写入本地文件
	clearRaftState();
	m_raftStateOutStream << data; 
	m_raftStateSize += data.size(); // 更新大小计数
}


// 读取 Raft 状态
std::string Persister::ReadRaftState() 
{
	std::lock_guard<std::mutex> lg(m_mtx); // 加锁，保证线程安全

	std::fstream ifs(m_raftStateFileName, std::ios_base::in);// 以只读模式打开状态文件
	if (!ifs.good()) 
	{
		return ""; // 如果打不开或文件不存在，返回空字符串
	}

	std::string raftState;
	ifs >> raftState; // 读取第一个空格或换行前的数据（格式是字符串）
	ifs.close();
	return raftState;
}

// 读取快照
std::string Persister::ReadSnapshot() 
{
	std::lock_guard<std::mutex> lg(m_mtx);
	if (m_snapshotOutStream.is_open()) // 如果快照输出流还在写，先关闭，避免读写冲突
	{
		m_snapshotOutStream.close();
	}	
	
	// 利用 DEFER 宏（延迟执行）读取完后重新打开写入流，恢复写入功能
	DEFER 
	{
		m_snapshotOutStream.open(m_snapshotFileName); // 在作用域结束时自动执行，默认是追加
	};
	
	std::fstream ifs(m_snapshotFileName, std::ios_base::in);
	if (!ifs.good()) 
	{
		return "";
	}
	std::string snapshot;
	ifs >> snapshot; // 读取第一个空格或换行前的字符串
	ifs.close();
	return snapshot; 
}



// 获取状态大小
long long Persister::RaftStateSize() 
{
	std::lock_guard<std::mutex> lg(m_mtx);
	return m_raftStateSize; // 提供当前状态所占用的大小，避免反复读取磁盘统计
}



// 清空状态文件
void Persister::clearRaftState() 
{
	// 状态大小清零
	m_raftStateSize = 0;
	// 关闭文件流
	if (m_raftStateOutStream.is_open()) 
	{
		m_raftStateOutStream.close();
	}
	// 重新打开文件流并清空文件内容
	m_raftStateOutStream.open(m_raftStateFileName, std::ios::out | std::ios::trunc);
}


// 清理快照文件
void Persister::clearSnapshot() 
{
  if (m_snapshotOutStream.is_open()) 
  {
  	m_snapshotOutStream.close();
  }
  m_snapshotOutStream.open(m_snapshotFileName, std::ios::out | std::ios::trunc);
}


// 同时清空状态和快照文件
void Persister::clearRaftStateAndSnapshot() 
{
	clearRaftState();
	clearSnapshot();
}