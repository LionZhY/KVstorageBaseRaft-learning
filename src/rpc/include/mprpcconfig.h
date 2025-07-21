#pragma once

#include <string>
#include <unordered_map>

// rpcserverip   rpcserverport    zookeeperip   zookeeperport
// 框架读取配置文件类


// MprpcConfig 负责从指定配置文件中读取参数，并将其保存在哈希表 m_configMap 中，供 RPC 框架中各模块统一访问。
class MprpcConfig 
{
public:
 	// 负责解析加载配置文件
 	void LoadConfigFile(const char *config_file);

 	// 查询配置项信息
 	std::string Load(const std::string &key);

private:
	std::unordered_map<std::string, std::string> m_configMap; // 存储所有配置项的哈希表

	// 去掉字符串前后的空格
	void Trim(std::string &src_buf);
};