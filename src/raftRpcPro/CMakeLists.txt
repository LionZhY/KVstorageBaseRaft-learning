set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 查找 protobuf
find_package(Protobuf REQUIRED)

message(STATUS "Protobuf_INCLUDE_DIRS = ${Protobuf_INCLUDE_DIRS}")
message(STATUS "Protobuf_LIBRARIES = ${Protobuf_LIBRARIES}")

# proto 文件
set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/kvServerRPC.proto)

# 自动生成 pb.h/pb.cc
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILE})

# 获取当前源文件所在目录的绝对路径
get_filename_component(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)
# 将该目录下所有源文件添加到 SRC_LIST 变量中
aux_source_directory(${SRC_DIR} SRC_LIST)


# 导出变量供上层使用
set(src_raftRpcPro ${SRC_LIST} CACHE INTERNAL "Description of the variable")


# ==== 添加 Protobuf 支持 BEGIN ====

# 创建静态库，包含所有源文件和 proto 生成的源文件
add_library(raftRpcPro
    ${SRC_LIST}
    ${PROTO_SRCS}
)

# 绑定头文件搜索路径到目标
target_include_directories(raftRpcPro
    PRIVATE
    /usr/include
    ${CMAKE_CURRENT_BINARY_DIR}
)

# 链接 protobuf 库
target_link_libraries(raftRpcPro ${Protobuf_LIBRARIES})

# ==== 添加 Protobuf 支持 END ====

# === 可执行文件部分 ===
add_executable(test test.cc)

target_include_directories(test
    PRIVATE
    /usr/include
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(test
    raftRpcPro
    ${Protobuf_LIBRARIES}
)