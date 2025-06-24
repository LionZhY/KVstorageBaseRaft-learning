#include "include/util.h"
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <iomanip>


// 带时间戳的格式化调试打印，受全局 Debug 控制
void DPrintf(const char *format, ...)
{
    if (Debug)
    {
        // 获取当前的日期，然后取日志信息，写入相应的日志文件当中 a+
        time_t now = time(nullptr);
        tm *nowtm = localtime(&now);
        va_list args;
        va_start(args, format);
        std::printf("[%d-%d-%d-%d-%d-%d] ", 
                     nowtm->tm_year + 1900, 
                     nowtm->tm_mon + 1, 
                     nowtm->tm_mday, 
                     nowtm->tm_hour,
                     nowtm->tm_min, nowtm->tm_sec);
        std::vprintf(format, args);
        std::printf("\n");
        va_end(args);
    }
}



// 断言检查
void myAssert(bool condition, std::string message) // 条件不满足时终止程序并打印错误
{
    if (!condition)
    {
        std::cerr << "Error: " << message << std::endl; 
        std::exit(EXIT_FAILURE);
    }
}


// 获取当前高精度时间点
std::chrono::_V2::system_clock::time_point now() { return std::chrono::high_resolution_clock::now(); }


// 生成随机的选举超时时间（毫秒级）
std::chrono::milliseconds getRandomizedElectionTimeout()
{
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<int> dist(minRandomizedElectionTime, maxRandomizedElectionTime);

    return std::chrono::milliseconds(dist(rng));
}


// 线程睡眠指定毫秒数
void sleepNMilliseconds(int N) { std::this_thread::sleep_for(std::chrono::milliseconds(N)); }




// 检测端口是否空闲可用
bool isReleasePort(unsigned short usPort) 
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP); // 创建一个TCP Socket (IPv4, 流式传输)

    sockaddr_in addr;               // 定义一个 IPv4 地址结构体
    addr.sin_family = AF_INET;      // 指定协议族为IPv4
    addr.sin_port = htons(usPort);  // 将端口号usPort转为网络字节序（大端）
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 设置绑定地址为本机回环地址 127.0.0.1

    // 将 socket 绑定到这个地址和端口
    int ret = ::bind(s, (sockaddr *)&addr, sizeof(addr)); 
    if (ret != 0) // 返回不是0，绑定失败，端口被占用，关闭Socket，返回false
    {
        close(s);
        return false;
    }
    close(s); // 返回 0，表示绑定成功 → 端口空闲，关闭Socket，返回true
    return true;
}


// 尝试获取一个空闲端口，最多尝试30次
bool getReleasePort(short &port)
{
	short num = 0;
	while (!isReleasePort(port) && num < 30) // 当前端口不可用且尝试次数未超 30
	{
		++port;
		++num;
	}
	if (num >= 30)
	{
		port = -1;
		return false;
	}
	return true;
}