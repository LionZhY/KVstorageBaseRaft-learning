#ifndef APPLYMSG_H
#define APPLYMSG_H

#include <string>

/*
ApplyMsg.h 用于 Raft 分布式一致性协议中的 消息传递结构定义 
定义了节点将日志条目或快照应用到状态机时的消息格式
*/


// ApplyMsg 封装 Raft 节点向上层（如 KV 状态机）发送的 “应用消息（Apply Message）”
class ApplyMsg 
{
public:
    bool CommandValid;   // 当前是否携带有效的日志命令
    std::string Command; // 具体的日志命令内容，通常是一个序列化后的字符串
    int CommandIndex;    // 该命令在 Raft 日志中的索引
     
    bool SnapshotValid;  // 当前是否携带快照
    std::string Snapshot;// 具体的快照数据
    int SnapshotTerm;    // 生成该快照时的任期号
    int SnapshotIndex;   // 该快照所代表的最后一个日志索引

public:
    //两个valid最开始要赋予false！！
    ApplyMsg()
        : CommandValid(false),
          Command(),
          CommandIndex(-1),
          SnapshotValid(false),
          SnapshotTerm(-1),
          SnapshotIndex(-1){
          };
};

#endif  // APPLYMSG_H