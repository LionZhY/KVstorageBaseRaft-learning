
# 获取当前源文件所在目录的绝对路径 存储到变量 SRC_DIR 中
get_filename_component(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)

# 将该目录 SRC_DIR 下所有源文件添加到 SRC_LIST 变量中
aux_source_directory(${SRC_DIR} SRC_LIST)



# 创建名为 rpc_lib 的 静态库 （包含当前目录的源码，common的源码）
add_library(rpc_lib ${SRC_LIST} ${src_common} ) 

# 为 rpc_lib 显式链接 外部库：boost_serialization
target_link_libraries(rpc_lib boost_serialization)



# 将当前模块源码 ${SRC_LIST} 存入变量 src_rpc，供其他模块引用该模块的源码
set(src_rpc ${SRC_LIST} CACHE INTERNAL "Description of the variable")