# 获取当前源文件所在目录的绝对路径 存到变量 SRC_DIR 中
get_filename_component(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)

# 将该目录 SRC_DIR 下所有源文件添加到 SRC_LIST 变量中
aux_source_directory(${SRC_DIR} SRC_LIST)

# 将当前模块源码 ${SRC_LIST} 存入变量 src_common，供其他模块引用该模块的源码
set(src_common ${SRC_LIST} CACHE INTERNAL "Description of the variable")