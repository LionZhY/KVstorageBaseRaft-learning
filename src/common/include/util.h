#ifndef UTIL_H
#define UTIL_H

/**
 * util 提供了程序运行中的基础工具函数，
 * 包括断言检查、获取当前时间、生成随机选举超时、线程睡眠、检测端口是否可用，以及调试日志的输出。
 */

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/serialization/access.hpp>
#include <condition_variable>  // pthread_condition_t
#include <functional>
#include <iostream>
#include <mutex>  // pthread_mutex_t
#include <queue>
#include <random>
#include <sstream>
#include <thread>
#include "config.h"



// 一些常量  是 KVServer 给客户端返回的操作结果状态码（reply）--------------------------------------
const std::string OK = "OK"; 						 // 操作成功
const std::string ErrNoKey = "ErrNoKey";			 // 对于get操作，请求的key不存在
const std::string ErrWrongLeader = "ErrWrongLeader"; // 当前处理节点不是raft的leader，不能处理请求




// 调试打印 ---------------------------------------------------------------------------------------
void DPrintf(const char* format, ...); // 带时间戳的格式化调试打印，受全局 Debug 控制


// 通用的字符串格式化函数模板 (类似于 printf) --------------------------------------------------------
template <typename... Args>
std::string format(const char *format_str, Args... args)
{
	int size_s = std::snprintf(nullptr, 0, format_str, args...) + 1; // "\0"
	if (size_s <= 0)
	{
		throw std::runtime_error("Error during formatting.");
	}
	auto size = static_cast<size_t>(size_s);
	std::vector<char> buf(size);
	std::snprintf(buf.data(), size, format_str, args...);
	return std::string(buf.data(), buf.data() + size - 1); // remove '\0'
}



// 断言检查 ---------------------------------------------------------------------------------------
void myAssert(bool condition, std::string message = "Assertion failed!"); // 条件不满足时终止程序并打印错误



// 时间相关  ----------------------------------------------------------------------------------------
std::chrono::_V2::system_clock::time_point now();         // 获取当前高精度时间点
std::chrono::milliseconds getRandomizedElectionTimeout(); // 生成随机的选举超时时间（毫秒级）
void sleepNMilliseconds(int N);                           // 线程睡眠指定毫秒数



// 端口检测与分配 --------------------------------------------------------------------------------
bool isReleasePort(unsigned short usPort); // 检测端口是否空闲可用
bool getReleasePort(short& port);          // 尝试获取一个空闲端口，最多尝试30次




// 延迟执行机制 --------------------------------------------------------------------------------

template <class F>
class DeferClass // 实现类似 Go 语言 defer 的功能，在作用域结束时自动执行指定代码
{
public:
	// 构造
	DeferClass(F &&f)      : m_func(std::forward<F>(f)) {}
	DeferClass(const F &f) : m_func(f) {}

	// 析构：延迟执行的关键，把代码封装进 m_func，当这个对象生命周期结束（比如函数体结束）时，会自动调用 m_func()
	~DeferClass() { m_func(); } 

	// 禁止拷贝构造和拷贝赋值 (保证每个延迟任务只执行一次)
	DeferClass(const DeferClass &e) = delete;
	DeferClass &operator=(const DeferClass &e) = delete;

private:
	F m_func; // 保存传进来的函数
};

#define _CONCAT(a, b) a##b // 宏拼接工具，a##b 表示将 a 和 b 拼接为一个新的名字
#define _MAKE_DEFER_(line) DeferClass _CONCAT(defer_placeholder, line) = [&]() // 创建一个 DeferClass 对象并传入一个 lambda 表达式（延迟执行的代码）

#undef DEFER // 先取消已有的 DEFER 定义（如果有）
#define DEFER _MAKE_DEFER_(__LINE__) // 定义 DEFER 宏：展开后会调用 _MAKE_DEFER_(__LINE__) 
									 // __LINE__ 是预定义宏，表示当前代码行号





// 线程安全队列 异步写日志的日志队列 --------------------------------------------------------------------

template <typename T>
class LockQueue // 基于互斥锁和条件变量实现的阻塞安全队列
{
public:
	// 线程安全入队  多个worker线程都会写日志queue
	void Push(const T &data)
	{
	  std::lock_guard<std::mutex> lock(m_mutex); // 使用 lock_guard 对互斥量加锁（RAII机制），防止同时有多个线程访问 m_queue
	  m_queue.push(data);
	  m_condvariable.notify_one(); // 唤醒一个等待线程（通常是 Pop() 中等待数据的线程）
	}

	// 线程安全出队（阻塞）  一个线程读日志queue，写日志文件
	T Pop()
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		while (m_queue.empty())
		{
			// 日志队列为空，线程进入wait状态，线程会阻塞，直到notify_one()唤醒
			m_condvariable.wait(lock); // 这里用unique_lock是因为lock_guard不支持解锁，而unique_lock支持
	  	}

		// 拿出队首元素并删除，返回给调用者
		T data = m_queue.front();
		m_queue.pop();
		return data;
	}

	// 带超时的pop
	bool timeOutPop(int timeout, T *ResData) // 添加一个超时时间参数，默认为 50 毫秒
	{
		std::unique_lock<std::mutex> lock(m_mutex);	
		
		// 获取当前时间点，并计算出超时时刻
		auto now = std::chrono::system_clock::now();
		auto timeout_time = now + std::chrono::milliseconds(timeout);	

		// 在超时之前，不断检查队列是否为空
		while (m_queue.empty())
		{
			// 如果已经超时了，就返回一个空对象
			if (m_condvariable.wait_until(lock, timeout_time) == std::cv_status::timeout)
			{
				return false;
			}
			else
			{
				continue;
			}
		}
		
		// 若成功取出数据，写入传入指针 ResData，返回 true
		T data = m_queue.front();
		m_queue.pop();
		*ResData = data;
		return true;
	}

private:
	std::queue<T> m_queue; // 存储实际数据的标准队列
	std::mutex m_mutex;
	std::condition_variable m_condvariable; // 条件变量，用于线程之间的阻塞等待 / 通知唤醒
};




// command 命令封装 ----------------------------------------------------------------------------------
class Op 
{		 
	/* 封装 [客户端对 kv 数据库的操作命令]，能变成字符串发给 Raft（序列化），还能还原回来执行（反序列化）*/
	/* 用来描述单次 KV 操作请求（如 Get / Put / Append） 的类对象，是kv传递给raft的command */

	/* 简单来说，Op 就是一条客户端对键值数据库的操作指令 */

public:
	// todo：为了协调raftRPC中的command只设置成了string,这个的限制就是正常字符中不能包含|
	// 当然后期可以换成更高级的序列化方法，比如protobuf

	// 将当前 Op 对象序列化成字符串
	std::string asString() const // 用于在 KVServer 发送 command 给 Raft 时，把结构体变成字符串传入
	{
		std::stringstream ss;
		boost::archive::text_oarchive oa(ss);	
		// write class instance to archive
		oa << *this;
		// close archive	
		return ss.str();
	}

	// 字符串反序列化为 Op 对象
	bool parseFromString(std::string str) // 用于 Raft 接收日志条目（string 类型）后，将其解析还原为 Op 命令结构
	{
		std::stringstream iss(str);
		boost::archive::text_iarchive ia(iss);
		// read class state from archive
		ia >> *this;
		return true; 
	}

public:
	// 运算符重载：支持 std::cout << Op
	friend std::ostream &operator<<(std::ostream &os, const Op &obj)
	{
		// 使得可以直接打印 Op 对象，输出格式如：[MyClass:Operation{Put},Key{foo},Value{bar},ClientId{cli123},RequestId{42}
		os << "[MyClass:Operation{" + obj.Operation + "},Key{" + obj.Key + "},Value{" + obj.Value + "},ClientId{" +
		          obj.ClientId + "},RequestId{" + std::to_string(obj.RequestId) + "}"; // 在这里实现自定义的输出格式
		return os;
	}


private:
	// 友元，让 Boost 的序列化框架 boost::serialization::access 类 可以访问 Op 的私有
	friend class boost::serialization::access; 
	
	// Boost 序列化函数 （Boost 要求每个可序列化的类实现一个 serialize() 方法）
	template <class Archive>
	void serialize(Archive &ar, const unsigned int version)
	{
		// 在这个函数里明确指出：你要序列化哪些成员变量
		// & 运算符表示「绑定」，根据是序列化器（写入）还是反序列化器（读取）自动决定方向，Boost 会自动推断这是序列化还是反序列化
		ar & Operation; 
		ar & Key;
		ar & Value;
		ar & ClientId;
		ar & RequestId;
	}

public:
	std::string Operation; // 操作类型"Get" "Put" "Append"
	std::string Key;		
	std::string Value;		
	std::string ClientId; // 发起请求的客户端id
	int RequestId;        // 客户端号码请求的Request的序列号，即第几个请求
};






// int main(int argc, char** argv)
//{
//     short port = 9060;
//     if(getReleasePort(port)) //在port的基础上获取一个可用的port
//     {
//         std::cout << "可用的端口号为：" << port << std::endl;
//     }
//     else
//     {
//         std::cout << "获取可用端口号失败！" << std::endl;
//     }
//     return 0;
// }

#endif  // UTIL_H