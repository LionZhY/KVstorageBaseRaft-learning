#include <iostream>
#include "raft.h"
// #include "kvServer.h"
#include <kvServer.h>   // kvServer.h 被安装到了系统路径或 -I 指定路径中 
#include <unistd.h>     // 提供 fork()、pause()、getopt() 等系统调用
#include <iostream>
#include <random>


/* 启动多个分布式 KVServer（Raft节点） 的示例，模拟 Raft 协议中的多个节点启动与端口配置 */


// 声明一个帮助函数，在下面实现，打印命令行参数格式提示
void ShowArgsHelp();


// 程序入口，接收命令行参数
int main(int argc, char **argv) // argc 参数数量，
{
    if (argc < 2) // 如果参数数量太少，提示格式并退出
    {
        ShowArgsHelp(); // 打印使用说明 如 "./raftKvDB -n 3 -f test.conf"
        exit(EXIT_FAILURE);
    }
    

    // 随机生成起始端口号 startPort（范围在 10000~29999），用于为每个 Raft 节点分配唯一端口
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(10000, 29999);
    unsigned short startPort = dis(gen);


    // 使用 getopt 解析参数： 如 "./raftKvDB -n 3 -f test.conf" （-n 节点数, -f 配置文件）
    int c = 0;
    int nodeNum = 0;            // 启动的 Raft 节点个数
    std::string configFileName; // 存储这些节点的配置信息（IP/端口）
    while ((c = getopt(argc, argv, "n:f:")) != -1) 
    {
        switch (c) {
            case 'n':   // -n：指定节点数量  3
                nodeNum = atoi(optarg);
                break;
            case 'f':   // -f：指定配置文件  test.conf
                configFileName = optarg;
                break;
            default:
                ShowArgsHelp();
                exit(EXIT_FAILURE);
        }
    }


    // 初始化配置文件
    std::ofstream file(configFileName, std::ios::out | std::ios::app); // 首先尝试以 append 模式打开文件，以保证其存在
    file.close();
    file = std::ofstream(configFileName, std::ios::out | std::ios::trunc);// 再以 truncate 模式清空文件内容，不保留旧信息，确保配置文件是干净的
    if (file.is_open()) { // 确认文件是否成功打开并清空，否则退出程序
        file.close();
        std::cout << configFileName << " 已清空" << std::endl;
    } else {
        std::cout << "无法打开 " << configFileName << std::endl;
        exit(EXIT_FAILURE);
    }


    // 启动 nodeNum 个节点
    for (int i = 0; i < nodeNum; i++) 
    {
        short port = startPort + static_cast<short>(i); // 每次循环生成一个新的端口
        std::cout << "start to create raftkv node:" << i 
                  << "    port:" << port 
                  << " pid:" << getpid() << std::endl;
        
        // 创建新子进程，子进程执行 Raft 节点逻辑
        pid_t pid = fork();  
        if (pid == 0)    // 如果是子进程
        {
            // 子进程中创建并启动 KvServer 节点 （每个节点编号为 i，快照大小限制 500，配置文件，本节点监听端口 port）
            auto kvServer = new KvServer(i, 500, configFileName, port); // 在这里写入配置文件
            // 进程阻塞等待，不会退出，不会执行 return             
            pause();   
        } 
        else if (pid > 0)// 如果是父进程 等待 1 秒，避免端口冲突或竞争
        {
            sleep(1); 
        } 
        else             // pid < 0 时，说明 fork 创建进程失败
        {   
            std::cerr << "Failed to create child process." << std::endl;
            exit(EXIT_FAILURE);
        }
    }


    // 主进程也进入暂停，等待信号或外部控制，确保子进程存活
    pause(); 

    return 0;
}



void ShowArgsHelp() 
{ 
    // 打印使用说明 如 "./raftKvDB -n 3 -f test.conf"  （-n 节点数, -f 配置文件）
    std::cout << "format: command -n <nodeNum> -f <configFileName>" << std::endl;  
}