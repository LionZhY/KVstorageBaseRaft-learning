# 获取当前源文件所在目录的绝对路径 SRC_DIR
get_filename_component(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)

# 将该目录下所有源文件添加到 SRC_LIST 变量中
aux_source_directory(${SRC_DIR} SRC_LIST)

# 将 SRC_LIST 的内容赋值给变量 src_skiplist ，确保变量在父级 CMake 中可见
set(src_skiplist ${SRC_LIST} CACHE INTERNAL "Description of the variable")