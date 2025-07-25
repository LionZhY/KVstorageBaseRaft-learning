# 定义变量 SRC_LIST，指定源文件 raftKvDB.cpp
set(SRC_LIST raftKvDB.cpp)

# 使用上述源文件 SRC_LIST 生成可执行文件 raftCoreRun —— 启动raft节点
add_executable(raftCoreRun ${SRC_LIST})
target_link_libraries(raftCoreRun skip_list_on_raft rpc_lib protobuf muduo_net muduo_base pthread )




# 定义变量 SRC_LIST2，指定源文件 caller.cpp
set(SRC_LIST2 caller.cpp)

# 使用 ${src_raftClerk} ${SRC_LIST2}  ${src_common} 创建可执行文件 callerMain —— 客户端测试
add_executable(callerMain ${src_raftClerk} ${SRC_LIST2}  ${src_common})
target_link_libraries(callerMain skip_list_on_raft  protobuf boost_serialization )