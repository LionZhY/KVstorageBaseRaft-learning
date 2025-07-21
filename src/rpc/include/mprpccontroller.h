#pragma once
#include <google/protobuf/service.h> // 包含 Protobuf 的核心服务定义头文件 service.h，它定义了 RpcController 类
#include <string>


// MprpcController 是自定义 RPC 框架中继承自 google::protobuf::RpcController 的派生类，
// 用于在一次 RPC 调用过程中传递 [控制信息]，如：是否失败、失败原因、取消请求等。

// 这类控制器在 Protobuf 的 RPC 框架中，是客户端与服务端通用的上下文载体，但多数功能是可选的，通常只在客户端使用。

class MprpcController : public google::protobuf::RpcController 
{
public:
    MprpcController();

    void Reset();                               // 重置控制器状态
    bool Failed() const;                        // 查询是否在调用过程中出现错误
    std::string ErrorText() const;              // 返回具体错误原因
    void SetFailed(const std::string& reason);  // 记录失败状态和描述信息

    // 目前未实现具体的功能
    void StartCancel();
    bool IsCanceled() const;
    void NotifyOnCancel(google::protobuf::Closure* callback);

private:
	bool m_failed;          // 标记此次 RPC 是否失败
	std::string m_errText;  // 若失败，保存失败原因
};