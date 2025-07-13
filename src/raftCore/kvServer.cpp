#include "kvServer.h"
#include <memory>
#include <rpcprovider.h>
#include <utility>
#include "ApplyMsg.h"
#include "config.h"
#include "mprpcconfig.h"
#include "raft.h"
#include "util.h"



KvServer::KvServer(int me,                          // 当前节点的id
                   int maxraftstate,                // 日志超过这个大小将触发快照
                   std::string nodeInforFileName,   // 节点网络配置文件路径
                   short port)                      // port 本节点监听的端口
    : m_skipList(6) // 初始化成员变量 m_skipList，传入参数 6 表示跳表的最大层数     
{
    // 状态初始化
    std::shared_ptr<Persister> persister = std::make_shared<Persister>(me); // 创建 Raft 状态持久化对象
    m_me = me;
    m_maxRaftState = maxraftstate;
    applyChan = std::make_shared<LockQueue<ApplyMsg>>(); // 创建 applyChan，用于 Raft 提交日志后向 KVServer 推送命令
    m_raftNode = std::make_shared<Raft>(); // 实例化 Raft 节点


    // 创建并启动一个子线程负责开启 RPC 服务
    std::thread t( [this, port]() -> void 
        {
            RpcProvider provider; // provider是一个rpc网络服务对象。把UserService对象发布到rpc节点上

            // clerk 层面 kvserver开启rpc接受功能，同时raft与raft节点之间也要开启rpc功能，因此有两个注册
            provider.NotifyService(this);                   // 将当前 KVServer 注册为 RPC 服务
            provider.NotifyService(this->m_raftNode.get()); // 将 Raft 节点也注册为 RPC 服务
            
            provider.Run(m_me, port); // 开启 RPC 监听服务，进程阻塞于此，等待远程Clerk/Raft的rpc调用请求
        } );

    t.detach(); // 该线程设置为后台运行


    /* 开启rpc远程调用能力，必须保证所有节点都开启rpc接受功能之后才能开启rpc远程调用能力，这里使用睡眠来保证 */ 
    std::cout << "raftServer node:" << m_me << " start to sleep to wait all ohter raftnode start!!!!" << std::endl;
    sleep(6); // 睡眠固定时间（6秒），确保其他节点 RPC 服务已启动
    std::cout << "raftServer node:" << m_me << " wake up!!!! start to connect other raftnode" << std::endl;


    // 从配置文件中读取所有 Raft 节点的 IP 和 端口信息，存入 ipPortVt
    MprpcConfig config;
    config.LoadConfigFile(nodeInforFileName.c_str());     // 加载指定配置文件（传入的文件名 nodeInforFileName）
    std::vector<std::pair<std::string, short> > ipPortVt; // 保存每个 Raft 节点的 IP 和端口
    for (int i = 0; i < INT_MAX - 1; ++i) 
    {
        std::string node = "node" + std::to_string(i);
        std::string nodeIp = config.Load(node + "ip");
        std::string nodePortStr = config.Load(node + "port");
        if (nodeIp.empty()) 
        {
          break;
        }
        ipPortVt.emplace_back(nodeIp, atoi(nodePortStr.c_str()));
    }


    // 创建对每个 “其他 Raft 节点” 的 RPC 客户端代理，存入 servers 
    std::vector<std::shared_ptr<RaftRpcUtil> > servers; // 其他 Raft 节点的 RPC 客户端代理 RaftRpcUtil
    for (int i = 0; i < ipPortVt.size(); ++i) //进行连接
    {
        if (i == m_me) // 本地节点跳过自己
        {
          servers.push_back(nullptr);
          continue;
        }
        std::string otherNodeIp = ipPortVt[i].first;
        short otherNodePort = ipPortVt[i].second;
        auto *rpc = new RaftRpcUtil(otherNodeIp, otherNodePort); // 构造一个新的 RaftRpcUtil 实例
        servers.push_back(std::shared_ptr<RaftRpcUtil>(rpc));    // 统一保存到 servers 向量中供 Raft 使用

        std::cout << "node" << m_me << " 连接node" << i << "success!" << std::endl;
    }


    // 初始化 Raft 节点
    sleep(ipPortVt.size() - me);  // 粗暴同步，确保所有节点相互连接成功，再启动raft
    m_raftNode->init(servers, m_me, persister, applyChan);


    // 状态恢复与快照安装
    m_lastSnapShotRaftLogIndex = 0; 
    auto snapshot = persister->ReadSnapshot();
    if (!snapshot.empty()) // 如果持久化中存在快照，调用 ReadSnapShotToInstall() 从快照恢复状态
    { 
        ReadSnapShotToInstall(snapshot);
    }


    // 启动【处理 Raft 提交日志】的主线程（阻塞，永久运行）
    std::thread t2(&KvServer::ReadRaftApplyCommandLoop, this); // 创建新线程，持续从 applyChan 中读取 Raft 日志并应用到状态机
    t2.join();  // 一旦启动，当前 KVServer 节点将持续运行直到程序终止，join() 表示该构造函数调用最终永远阻塞在此（主线程阻塞）
}



// 打印当前 kv 数据，调试用
void KvServer::DprintfKVDB()
{
    if (!Debug) {
        return;
    }

    std::lock_guard<std::mutex> lg(m_mtx);
    DEFER 
    {
        m_skipList.display_list(); // 输出当前跳表中所有键值对的结构
    };
}



///////////////////////////////////////////  客户端请求执行  ///////////////////////////////////////////

// 执行 Clerk 客户端提交的 Get 操作
void KvServer::ExecuteGetOpOnKVDB(Op op, std::string *value, bool *exist) 
{
    /* 读取指定 key 的值 value，返回是否命中（exist） */
    m_mtx.lock();

    // 初始化返回值
    *value = "";
    *exist = false;

    // 从跳表中查找 key
    if (m_skipList.search_element(op.Key, *value))
    {
        *exist = true; // 如果找到了，将值写入 *value，并设置 *exist = true
                       // value已经在 search_element() 完成赋值了
    }

    m_lastRequestId[op.ClientId] = op.RequestId; // 记录客户端最新请求 ID

    m_mtx.unlock();
    DprintfKVDB();
}


// 执行 Clerk 客户端提交的 Put 操作
void KvServer::ExecutePutOpOnKVDB(Op op) 
{
    /* 将指定 op.Key 对应的值更新为 op.Value，如果 key 不存在就插入 */
    m_mtx.lock();

    m_skipList.insert_set_element(op.Key, op.Value); // 覆盖性插入跳表
    m_lastRequestId[op.ClientId] = op.RequestId;     // 记录客户端最新请求 ID
    
    m_mtx.unlock();
    DprintfKVDB();
}


// 执行 Clerk 客户端提交的 Append 操作
void KvServer::ExecuteAppendOpOnKVDB(Op op) 
{
    /* 在当前 Key 对应的旧值基础上追加拼接 op.Value */ 

    m_mtx.lock();

    // m_skipList.insert_set_element(op.Key, op.Value); // 覆盖性插入跳表
    // m_lastRequestId[op.ClientId] = op.RequestId; // 记录客户端最新请求 ID（此客户端 ClientId 最后一个已成功执行的请求是 RequestId）
    
    // 原先上面那种写法和put实现的一样，应该是有问题，put是覆盖，append是追加拼接

    std::string oldValue;
    
    if (m_skipList.search_element(op.Key, oldValue)) 
    {
        // key 已存在，执行追加 (拼接)
        std::string newValue = oldValue + op.Value; // 新值
        m_skipList.insert_set_element(op.Key, newValue); 
    } 
    else 
    {
        // key 不存在，等效于 put，插入
        m_skipList.insert_set_element(op.Key, op.Value);
    }

    m_lastRequestId[op.ClientId] = op.RequestId; // 记录客户端最新请求 ID

    m_mtx.unlock();
    DprintfKVDB();
}




// 判断请求是否是重复提交，用于实现幂等性
bool KvServer::ifRequestDuplicate(std::string ClientId, int RequestId)
{
    std::lock_guard<std::mutex> lg(m_mtx);

    // 记录表中找不到这个客户端的 ID，说明这是该客户端第一次访问
    if (m_lastRequestId.find(ClientId) == m_lastRequestId.end()) 
    {
        return false;
    }

    // 如果该客户端存在，进一步判断当前请求 RequestId 小于或等于记录中已处理的最大值
    return RequestId <= m_lastRequestId[ClientId];
}





/////////////////////////////////////////   与 raft 交互日志处理   /////////////////////////////////////////

// 处理 Raft 层提交的日志（ApplyMsg）
void KvServer::GetCommandFromRaft(ApplyMsg message)
{
    /*  
        处理 Raft 层提交的日志（ApplyMsg），并驱动本地状态机（跳表）执行命令，
        必要时触发快照，并通知等待的 Clerk 客户端。
    */
    
    // Op op             是原始来自 Clerk 客户端的请求命令 
    //                   Clerk → KVServer（Put/Get/Append 请求）    Clerk 构造一个 Op 对象

    // ApplyMsg message  Raft 节点传给 KVServer 的              
    //                   raft 层收到command被raft commit后，raft  → KVServer（Clerk 原始请求 Op 的序列化结果） 
    
    
    // 从 message.Command 反序列化恢复出原始的 Op
    Op op;
    op.parseFromString(message.Command); 

    DPrintf("[KvServer::GetCommandFromRaft-kvserver{%d}] , Got Command --> Index:{%d} , ClientId {%s}, RequestId {%d}, "
            "Opreation {%s}, Key :{%s}, Value :{%s}",
            m_me, message.CommandIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);
    
    // 如果当前日志索引小于等于快照点，说明日志内容已包含在快照中，不再重复执行
    if (message.CommandIndex <= m_lastSnapShotRaftLogIndex) return;


    // 如果请求不是重复的，执行具体操作
    if (!ifRequestDuplicate(op.ClientId, op.RequestId))
    {
        // 根据操作类型，调用对应的逻辑
        if (op.Operation == "Put")      ExecutePutOpOnKVDB(op);
        if (op.Operation == "Append")   ExecuteAppendOpOnKVDB(op);
    }


    // 如果启用了日志上限（m_maxRaftState != -1）
    if (m_maxRaftState != -1)
    {
        // 判断是否需要制作快照（例如 log 体积超过上限的 1/9）
        IfNeedToSendSnapShotCommand(message.CommandIndex, 9);
    }

     // 通知等待的 Clerk 客户端
     SendMessageToWaitChan(op, message.CommandIndex);
}



// 操作结果写回 clerk 等待通道 waitApplyCh
bool KvServer::SendMessageToWaitChan(const Op &op, 
                                     int raftIndex) // 该命令对应的 Raft 日志索引
{
    /* 操作结果 “回传” 给等待响应的 Clerk 请求线程 */
    std::lock_guard<std::mutex> lg(m_mtx);

    DPrintf("[RaftApplyMessageSendToWaitChan--> raftserver{%d}] , Send Command --> Index:{%d} , ClientId {%d}, RequestId "
            "{%d}, Opreation {%v}, Key :{%v}, Value :{%v}",
            m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);

    // 检查该 raftIndex 对应的 Clerk 是否仍在等待回复
    if (waitApplyCh.find(raftIndex) == waitApplyCh.end())   return false;
    
    // 将操作 op 推入对应的 LockQueue<Op> 中，通知 Clerk 命令已被 Raft 成功提交并执行完毕
    waitApplyCh[raftIndex]->Push(op);

    DPrintf("[RaftApplyMessageSendToWaitChan--> raftserver{%d}] , Send Command --> Index:{%d} , ClientId {%d}, RequestId "
            "{%d}, Opreation {%v}, Key :{%v}, Value :{%v}",
            m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);
    
    return true;
}




// 持续从 applyChan 获取 Raft 提交的日志项 ApplyMsg，调用具体处理
void KvServer::ReadRaftApplyCommandLoop()
{
    // 无限循环，用于后台持续运行，在KVServer构造末尾开启新线程永久运行
    while (true)
    {    
        // 从 applyChan 取出来自 Raft 层已提交的日志或快照
        auto message = applyChan->Pop(); // 阻塞弹出，Pop() 会阻塞直到有新消息可读

        DPrintf("---------------tmp-------------[func-KvServer::ReadRaftApplyCommandLoop()-kvserver{%d}] 收到了下raft的消息",
                m_me);
        
        // listen to every command applied by its raft, delivery to relative RPC Handler

        // 判断消息类型，调用处理
        if (message.CommandValid)
        {
            GetCommandFromRaft(message);  // 如果是有效日志命令，则交给状态机处理
        }
        if (message.SnapshotValid)
        {
            GetSnapShotFromRaft(message); // 如果是快照数据，则进行快照恢复
        }
    }

}




/////////////////////////////////////////  Clerk请求的 RPC 接口  /////////////////////////////////////////

// 处理来自 clerk 的 Get RPC 请求
void KvServer::Get(const raftKVRpcProctoc::GetArgs *args, // Get rpc 请求
                   raftKVRpcProctoc::GetReply *reply)     // Get rpc 回复   
{
    /* 从 Clerk 接收 Get(key) 请求 → 封装Op → 提交给 Raft → 等待日志被 commit → 查询kv跳表 → 返回结果给 Clerk */
    
    // 封装请求 args 为 Op 命令对象
    Op op;
    op.Operation = "Get";
    op.Key = args->key();// 查询的key
    op.Value = "";       // Get 请求不用携带 value
    op.ClientId = args->clientid();   // 请求来源客户端 ID
    op.RequestId = args->requestid(); // 请求编号，用于幂等性判断


    // Raft::Start() 提交日志给 Raft 节点
    int raftIndex = -1; // 当前 op 预计会在日志中的索引
    int _ = -1;
    bool isLeader = false;
    m_raftNode->Start(op, &raftIndex, &_, &isLeader); // 如果当前节点是 Leader，Start() 会返回预测的 raftIndex
                                                      // 虽然是预计，但是正确情况下是准确的，op的具体内容对raft来说 是隔离的
   
                        

    // 如果不是 Leader，就直接返回错误给 Clerk，让 Clerk 重新尝试另一个节点
    if (!isLeader)
    {
        reply->set_err(ErrWrongLeader);
        return;
    }


    // 为本次日志创建等待通道 chForRaftIndex
    m_mtx.lock();

    if (waitApplyCh.find(raftIndex) == waitApplyCh.end()) 
    {
        // 创建新的 LockQueue<Op> 用于等待 raftIndex 这条日志的 commit
        waitApplyCh.insert(std::make_pair(raftIndex, new LockQueue<Op>()));
    }
    auto chForRaftIndex = waitApplyCh[raftIndex]; 

    m_mtx.unlock(); // 直接解锁，等待任务执行完成，不能一直拿锁等待



    // 阻塞等待 Raft 日志被 commit
    Op raftCommitOp;
    if (!chForRaftIndex->timeOutPop(CONSENSUS_TIMEOUT, &raftCommitOp)) 
    {
        // 如果超时没收到提交（即 Raft 共识失败或太慢），进入处理超时逻辑

        int _ = -1;
        bool isLeader = false;
        m_raftNode->GetState(&_, &isLeader); // 只关注是不是Leader

        // 幂等重试：检查请求是否重复提交，以及是否是 leader
        if (ifRequestDuplicate(op.ClientId, op.RequestId) && isLeader)
        {
            /* Raft 这次没有 commit 成功，但如果之前成功执行过同一个 Get 请求，是可以再执行的，不会违反一致性*/
            std::string value;
            bool exist = false;
            ExecuteGetOpOnKVDB(op, &value, &exist); // 再次执行Get

            if (exist) // 如果 key 存在，就正常返回
            {
                reply->set_err(OK);
                reply->set_value(value);
            }
            else       // 如果 key 不存在，也要返回（ErrNoKey），这本身也是一致状态的一部分
            {
                reply->set_err(ErrNoKey);
                reply->set_value("");
            }
        }
        else // 当前节点不是 Leader，或者该请求从没成功处理过，返回错误，clerk会尝试访问其他节点
        {
            reply->set_err(ErrWrongLeader); 
        }

    }
    else // timeOutPop() 成功获取到了 Raft 通过 applyChan 提交的日志，共识成功，可以执行操作
    {        
        // 验证：这个日志是否确实是我现在处理的 op，防止错响应
        if (raftCommitOp.ClientId == op.ClientId && raftCommitOp.RequestId == op.RequestId)
        {
            std::string value;
            bool exist = false;
            ExecuteGetOpOnKVDB(op, &value, &exist); // 真正从本地状态机（跳表）中执行 Get 操作
            if (exist)
            {
                reply->set_err(OK);
                reply->set_value(value);
            }
            else
            {
                reply->set_err(ErrNoKey);
                reply->set_value("");
            }
        }
        else  // raftCommitOp 和原请求 op 不一致 (可能有别的请求抢占了日志槽位)
        {
            reply->set_err(ErrWrongLeader);
        }

    }


    // 清理资源
    m_mtx.lock(); 

    auto tmp = waitApplyCh[raftIndex]; 
    waitApplyCh.erase(raftIndex); // 删除临时等待通道
    delete tmp;

    m_mtx.unlock();
}




// 处理来自clerk的PutAppend RPC
void KvServer::PutAppend(const raftKVRpcProctoc::PutAppendArgs *args, 
                         raftKVRpcProctoc::PutAppendReply *reply) 
{
    // get和put//append執行的具體細節是不一樣的
    // PutAppend在收到raft消息之後執行，具體函數裏面只判斷冪等性（是否重複）
    // get函數收到raft消息之後在，因爲get無論是否重複都可以再執行
    Op op;
    op.Operation = args->op();
    op.Key = args->key();
    op.Value = args->value();
    op.ClientId = args->clientid();
    op.RequestId = args->requestid();

    int raftIndex = -1;
    int _ = -1;
    bool isleader = false;

    m_raftNode->Start(op, &raftIndex, &_, &isleader);

    if (!isleader) 
    {
        DPrintf("[func -KvServer::PutAppend -kvserver{%d}]From Client %s (Request %d) To Server %d, key %s, raftIndex %d , but "
                "not leader",
                m_me, &args->clientid(), args->requestid(), m_me, &op.Key, raftIndex);

        reply->set_err(ErrWrongLeader);
        return;
    }

    DPrintf("[func -KvServer::PutAppend -kvserver{%d}]From Client %s (Request %d) To Server %d, key %s, raftIndex %d , is "
            "leader ",
            m_me, &args->clientid(), args->requestid(), m_me, &op.Key, raftIndex);
    


    m_mtx.lock();
    if (waitApplyCh.find(raftIndex) == waitApplyCh.end()) 
    {
        waitApplyCh.insert(std::make_pair(raftIndex, new LockQueue<Op>()));
    }
    auto chForRaftIndex = waitApplyCh[raftIndex];

    m_mtx.unlock();  //直接解锁，等待任务执行完成，不能一直拿锁等待



    // timeout
    Op raftCommitOp;
    if (!chForRaftIndex->timeOutPop(CONSENSUS_TIMEOUT, &raftCommitOp)) 
    {
        DPrintf("[func -KvServer::PutAppend -kvserver{%d}]TIMEOUT PUTAPPEND !!!! Server %d , get Command <-- Index:%d , "
                "ClientId %s, RequestId %s, Opreation %s Key :%s, Value :%s",
                m_me, m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);

        if (ifRequestDuplicate(op.ClientId, op.RequestId)) 
        {
            reply->set_err(OK);  // 超时了,但因为是重复的请求，返回ok，实际上就算没有超时，在真正执行的时候也要判断是否重复
        } 
        else 
        {
            reply->set_err(ErrWrongLeader);  ///这里返回这个的目的让clerk重新尝试
        }
    } 
    else 
    {
        DPrintf("[func -KvServer::PutAppend -kvserver{%d}]WaitChanGetRaftApplyMessage<--Server %d , get Command <-- Index:%d , "
                "ClientId %s, RequestId %d, Opreation %s, Key :%s, Value :%s",
                m_me, m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);
        
        if (raftCommitOp.ClientId == op.ClientId && raftCommitOp.RequestId == op.RequestId) 
        {
            //可能发生leader的变更导致日志被覆盖，因此必须检查
            reply->set_err(OK);
        } 
        else 
        {
            reply->set_err(ErrWrongLeader);
        }
    }

    m_mtx.lock();
    auto tmp = waitApplyCh[raftIndex];
    waitApplyCh.erase(raftIndex);
    delete tmp;
    m_mtx.unlock();
}







////////////////////////////////////////////  快照机制支持  ////////////////////////////////////////////

void KvServer::IfNeedToSendSnapShotCommand(int raftIndex, int proportion)
{


}

std::string KvServer::MakeSnapShot() 
{

}



void KvServer::ReadSnapShotToInstall(std::string snapshot) 
{

}


void KvServer::GetSnapShotFromRaft(ApplyMsg message) 
{

}



//////////////////////////////////////////  RPC 对外接口重写  //////////////////////////////////////////

// 对外接口（对clerk）  PutAppend / Get（重载版本，供 Protobuf RPC 调用）
void KvServer::PutAppend(google::protobuf::RpcController *controller, 
                         const ::raftKVRpcProctoc::PutAppendArgs *request,
                         ::raftKVRpcProctoc::PutAppendReply *response, 
                         ::google::protobuf::Closure *done) 
{

}

void KvServer::Get(google::protobuf::RpcController *controller, 
                   const ::raftKVRpcProctoc::GetArgs *request,
                   ::raftKVRpcProctoc::GetReply *response, 
                   ::google::protobuf::Closure *done) 
{

}










