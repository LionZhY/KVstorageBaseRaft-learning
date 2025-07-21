#include "mprpccontroller.h"


MprpcController::MprpcController() 
{
	m_failed = false; // 初始化为无失败状态
	m_errText = "";
}

// 重置控制器状态
void MprpcController::Reset() 
{
	m_failed = false;
	m_errText = "";
}


// 查询是否在调用过程中出现错误
bool MprpcController::Failed() const { return m_failed; }


// 返回具体错误原因
std::string MprpcController::ErrorText() const { return m_errText; }


// 记录失败状态和描述信息
void MprpcController::SetFailed(const std::string& reason) 
{
	m_failed = true;
	m_errText = reason;
}



// 目前未实现具体的功能
void MprpcController::StartCancel() {}
bool MprpcController::IsCanceled() const { return false; }
void MprpcController::NotifyOnCancel(google::protobuf::Closure* callback) {}