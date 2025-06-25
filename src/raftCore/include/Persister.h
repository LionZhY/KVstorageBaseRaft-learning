#ifndef SKIP_LIST_ON_RAFT_PERSISTER_H
#define SKIP_LIST_ON_RAFT_PERSISTER_H

#include <fstream>
#include <mutex>

class Persister 
{
private:
	std::mutex m_mtx;
	std::string m_raftState; // 当前raft状态（如日志等）的内存副本
	std::string m_snapshot;	 // 当前snapshot快照的内存副本

	const std::string m_raftStateFileName; // raft状态的持久化文件名
	const std::string m_snapshotFileName;  // snapshot 快照的持久化文件名

	std::ofstream m_raftStateOutStream; // 保存raft 状态的输出文件流
	std::ofstream m_snapshotOutStream;  // 保存snapshot的输出流

	long long m_raftStateSize; // 保存raft 状态的字节数，用于状态量查询，避免每次都读取文件来获取具体的大小

public:

	// 构造和析构：初始化与关闭相关文件资源
	explicit Persister(int me); // me 是节点编号，生成该节点独立的文件名
	~Persister();

	// 一次性保存raft状态和快照
	void Save(std::string raftstate, std::string snapshot);

	// 保存单独raft状态
	void SaveRaftState(const std::string &data);

	// 读取状态
	std::string ReadRaftState();
	// 读取快照
	std::string ReadSnapshot();

	
	
	// 获取状态大小
	long long RaftStateSize();

private:
	// 清理
	void clearRaftState();
	void clearSnapshot();
	void clearRaftStateAndSnapshot();
};

#endif // SKIP_LIST_ON_RAFT_PERSISTER_H