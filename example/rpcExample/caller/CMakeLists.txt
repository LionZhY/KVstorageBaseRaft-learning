
set(SRC_LIST callFriendService.cpp ../friend.pb.cc)


# 创建名为 consumer 的可执行文件
add_executable(consumer ${SRC_LIST})
target_link_libraries(consumer rpc_lib protobuf)