# 获取当前源文件所在目录的绝对路径 存储到变量 SRC_DIR 中
get_filename_component(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)

# 将该目录下所有源文件添加到 SRC_LIST 变量中
aux_source_directory(${SRC_DIR} SRC_LIST)


# 创建名为 fiber_lib 的 静态库
add_library(fiber_lib ${SRC_LIST})

# 为 rpc_lib 显式链接 外部库：libdl
target_link_libraries(fiber_lib -ldl)


# 将当前模块源码 ${SRC_LIST} 存入变量 src_fiber，供其他模块引用该模块的源码
set(src_fiber ${SRC_LIST} CACHE INTERNAL "Description of the variable")