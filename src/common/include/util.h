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



// 线程安全队列 -------------------------------------------------------------------------------------
// //////////////////////// 异步写日志的日志队列 ////////////////////////
// read is blocking!!! LIKE  go chan
template <typename T>
class LockQueue // 基于互斥锁和条件变量实现的阻塞安全队列
{
public:
	// 多个worker线程都会写日志queue
	void Push(const T &data)
	{
	  std::lock_guard<std::mutex> lock(m_mutex); // 使用lock_gurad，即RAII的思想保证锁正确释放
	  m_queue.push(data);
	  m_condvariable.notify_one();
	}

	// 一个线程读日志queue，写日志文件
	T Pop()
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		while (m_queue.empty())
		{
			// 日志队列为空，线程进入wait状态
			m_condvariable.wait(lock); // 这里用unique_lock是因为lock_guard不支持解锁，而unique_lock支持
	  	}
		T data = m_queue.front();
		m_queue.pop();
		return data;
	}

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
		T data = m_queue.front();
		m_queue.pop();
		*ResData = data;
		return true;
	}

private:
	std::queue<T> m_queue;
	std::mutex m_mutex;
	std::condition_variable m_condvariable;
};

// 两个对锁的管理用到了RAII的思想，防止中途出现问题而导致资源无法释放的问题！！！
// std::lock_guard 和 std::unique_lock 都是 C++11 中用来管理互斥锁的工具类，它们都封装了 RAII（Resource Acquisition Is
// Initialization）技术，使得互斥锁在需要时自动加锁，在不需要时自动解锁，从而避免了很多手动加锁和解锁的繁琐操作。
// std::lock_guard 是一个模板类，它的模板参数是一个互斥量类型。当创建一个 std::lock_guard
// 对象时，它会自动地对传入的互斥量进行加锁操作，并在该对象被销毁时对互斥量进行自动解锁操作。std::lock_guard
// 不能手动释放锁，因为其所提供的锁的生命周期与其绑定对象的生命周期一致。 std::unique_lock
// 也是一个模板类，同样的，其模板参数也是互斥量类型。不同的是，std::unique_lock 提供了更灵活的锁管理功能。可以通过
// lock()、unlock()、try_lock() 等方法手动控制锁的状态。当然，std::unique_lock 也支持 RAII
// 技术，即在对象被销毁时会自动解锁。另外， std::unique_lock 还支持超时等待和可中断等待的操作。




// 数据序列化与命令封装 ----------------------------------------------------------------------------------
class Op // 封装 KV 操作命令，支持基于 Boost 序列化的字符串序列化与反序列化。(这个Op是kv传递给raft的command)
{
public:
	// Your definitions here.
	// Field names must start with capital letters,
	// otherwise RPC will break.
	std::string Operation; // "Get" "Put" "Append"
	std::string Key;
	std::string Value;
	std::string ClientId; // 客户端号码
	int RequestId;        // 客户端号码请求的Request的序列号，为了保证线性一致性
                 //  IfDuplicate bool // Duplicate command can't be applied twice , but only for PUT and APPEND

public:
	// todo
	// 为了协调raftRPC中的command只设置成了string,这个的限制就是正常字符中不能包含|
	// 当然后期可以换成更高级的序列化方法，比如protobuf
	std::string asString() const
	{
		std::stringstream ss;
		boost::archive::text_oarchive oa(ss);	
		// write class instance to archive
		oa << *this;
		// close archive	
		return ss.str();
	}

	bool parseFromString(std::string str)
	{
		std::stringstream iss(str);
		boost::archive::text_iarchive ia(iss);
		// read class state from archive
		ia >> *this;
		return true; // todo : 解析失敗如何處理，要看一下boost庫了
	}

public:
	friend std::ostream &operator<<(std::ostream &os, const Op &obj)
	{
	  os << "[MyClass:Operation{" + obj.Operation + "},Key{" + obj.Key + "},Value{" + obj.Value + "},ClientId{" +
	            obj.ClientId + "},RequestId{" + std::to_string(obj.RequestId) + "}"; // 在这里实现自定义的输出格式
	  return os;
	}

private:
	friend class boost::serialization::access;
	template <class Archive>
	void serialize(Archive &ar, const unsigned int version)
	{
		ar & Operation;
		ar & Key;
		ar & Value;
		ar & ClientId;
		ar & RequestId;
	}
};


// 一些常量 -------------------------------------------------------------------------------------
const std::string OK = "OK";
const std::string ErrNoKey = "ErrNoKey";
const std::string ErrWrongLeader = "ErrWrongLeader";



// 端口检测与分配 --------------------------------------------------------------------------------
bool isReleasePort(unsigned short usPort);// 检测端口是否空闲可用
bool getReleasePort(short& port);         // 尝试获取一个空闲端口，最多尝试30次



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