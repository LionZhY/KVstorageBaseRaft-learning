
# 设置变量 SRC_LIST，包含编译的源文件列表
set(SRC_LIST friendService.cpp ../friend.pb.cc)


# 创建名为 provider 的可执行文件
add_executable(provider ${SRC_LIST})
# provider 链接库
target_link_libraries(provider rpc_lib protobuf muduo_net muduo_base pthread)