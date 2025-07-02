#ifndef CONFIG_H
#define CONFIG_H

/**
 * config.h 集中定义分布式系统运行中的 [全局配置参数] ：调试控制、Raft 定时机制、协程运行设置
 * 
 */


// 调试相关
const bool Debug = true; // 是否开启调试模式
const int debugMul = 1;  // 时间倍率系数，用于统一调整所有定时参数 (不同网络环境rpc速度不同，因此需要乘以一个系数)


// Raft 协议定时参数（ms）
const int HeartBeatTimeout = 25 * debugMul;// 心跳发送间隔（= 25 × debugMul）(心跳时间一般要比选举超时小一个数量级)
const int ApplyInterval = 10 * debugMul;   // Apply 状态机的检查间隔（= 10 × debugMul）

const int minRandomizedElectionTime = 300 * debugMul;  // 选举超时最小值（= 300 × debugMul）
const int maxRandomizedElectionTime = 500 * debugMul;  // 选举超时最大值（= 500 × debugMul）

const int CONSENSUS_TIMEOUT = 500 * debugMul;  // 达成共识的超时时间（= 500 × debugMul）


// 协程调度参数
const int FIBER_THREAD_NUM = 1;              // 协程线程池大小（设为 1）
const bool FIBER_USE_CALLER_THREAD = false;  // 是否使用调用线程caller_thread执行调度任务（false）



#endif  // CONFIG_H