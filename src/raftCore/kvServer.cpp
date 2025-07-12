#include "kvServer.h"
#include <memory>
#include <rpcprovider.h>
#include "ApplyMsg.h"
#include "mprpcconfig.h"
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
    m_lastRequestId[op.ClientId] = op.RequestId; // 记录客户端最新请求 ID
    
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



///////////////////////////////////////////   与 raft 交互   ///////////////////////////////////////////

// Raft commit 某条日志后回调，用于驱动状态机执行
void KvServer::GetCommandFromRaft(ApplyMsg message)
{



}



// 提交结果写回等待通道
bool KvServer::SendMessageToWaitChan(const Op &op, int raftIndex) 
{


}


// 持续从 applyChan 获取 Raft 提交的日志项
void KvServer::ReadRaftApplyCommandLoop()
{



}




/////////////////////////////////////////  Clerk 触发的远程请求  /////////////////////////////////////////

// clerk 使用RPC远程调用 （	调用 Raft::Start 提交操作；收到 commit 后响应 Clerk ）
void KvServer::PutAppend(const raftKVRpcProctoc::PutAppendArgs *args, 
                         raftKVRpcProctoc::PutAppendReply *reply) 
{


}


void KvServer::Get(const raftKVRpcProctoc::GetArgs *args, 
                   raftKVRpcProctoc::GetReply *reply)
{

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










