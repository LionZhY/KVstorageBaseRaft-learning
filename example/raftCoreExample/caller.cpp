#include <iostream>
#include "clerk.h"
#include "util.h"


/* 客户端测试程序 */
/* 模拟一个客户端持续向集群发送 Put 和 Get 请求，验证系统的写入和读取一致性 */


int main()
{
    Clerk client; // 创建一个 Clerk 客户端对象，封装了对多个 KVServer 节点的连接与请求转发逻辑
    client.Init("test.conf"); // 初始化和raft节点的连接，配置文件是test.conf，根据配置文件建立RPC通道

    auto start = now(); // 没用上

    int count = 500; // 设定要执行 500 次操作
    int tmp = count;
    while (tmp--)
    {
        // 向kv集群写入键值对 <x, tmp> 每次写入都是覆盖 "x" 这个键的值
        client.Put("x", std::to_string(tmp)); 
        
        // 立即读取键 "x" 的值，打印返回字符串结果 理论上等于刚写入的 tmp
        std::string get1 = client.Get("x");   
        std::printf("get return :{%s}\r\n", get1.c_str());
    }

    return 0;
}