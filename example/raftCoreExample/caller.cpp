#include <iostream>
#include "clerk.h"
#include "util.h"


/* 测试 Raft + KVServer 集群功能的客户端测试程序 */
/* 模拟一个客户端持续向集群发送 Put 和 Get 请求，验证系统的写入和读取一致性 */


int main()
{
    Clerk client; // 创建一个 Clerk 客户端对象
    client.Init("test.conf"); // 初始化和raft节点的连接，配置文件名是test.conf

    auto start = now();
    int count = 500; // 设定要执行 500 次操作
    int tmp = count;
    while (tmp--)
    {
        client.Put("x", std::to_string(tmp)); // 向kv集群写入键值对 <x, tmp> 每次写入都是覆盖 "x" 这个键的值
        
        std::string get1 = client.Get("x");   // 立即读取键 "x" 的值，返回字符串结果 理论上等于刚写入的 tmp
        std::printf("get return :{%s}\r\n", get1.c_str());
    }

    return 0;
}