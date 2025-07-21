#include "mprpcconfig.h"

#include <iostream>
#include <string>



// 负责解析加载配置文件
void MprpcConfig::LoadConfigFile(const char *config_file) 
{
	// 打开配置文件
	FILE *pf = fopen(config_file, "r");
	if (nullptr == pf) {
		std::cout << config_file << " is note exist!" << std::endl;
		exit(EXIT_FAILURE);
	}

	// 逐行读取文件内容
	// 1.注释   2.正确的配置项 =    3.去掉开头的多余的空格
	while (!feof(pf)) 
	{
		char buf[512] = {0};
		fgets(buf, 512, pf); // 读一行配置

		std::string read_buf(buf);
		Trim(read_buf); // 去掉字符串前面多余的空格

		if (read_buf[0] == '#' || read_buf.empty()) { // 跳过注释# 和空行
			continue;
		}

		// 解析配置项
		int idx = read_buf.find('='); // 查找等号（=）分隔符
		if (idx == -1) {
			continue; // 没有 "=" 说明配置项不合法
		}

		// 拆分 key  value
		std::string key;
		std::string value;
		key = read_buf.substr(0, idx);
		Trim(key); // 去掉 key 两端空格

		// rpcserverip=127.0.0.1\n
		int endidx = read_buf.find('\n', idx);
		value = read_buf.substr(idx + 1, endidx - idx - 1);
		Trim(value);

		// 存入哈希map
		m_configMap.insert({key, value});
	}

	fclose(pf);
}



// 根据 key 查询配置项信息 value 
std::string MprpcConfig::Load(const std::string &key) 
{
	auto it = m_configMap.find(key);
	if (it == m_configMap.end()) {
		return "";
	}
	return it->second;
}



// 去掉字符串前后的空格
void MprpcConfig::Trim(std::string &src_buf) 
{
	int idx = src_buf.find_first_not_of(' '); // 先找第一个非空格字符，再裁剪前缀空格
	if (idx != -1) {
		src_buf = src_buf.substr(idx, src_buf.size() - idx);
	}

	idx = src_buf.find_last_not_of(' '); // 再找最后一个非空格字符，裁剪后缀空格
	if (idx != -1) {
		src_buf = src_buf.substr(0, idx + 1);
	}
}