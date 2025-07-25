# 设置cmake的最低版本
cmake_minimum_required(VERSION 3.22)
# 项目名称
project(KVRaftCpp) 


# 全局编译配置
set(CMAKE_CXX_STANDARD 20) # 使用C++20标准
set(CMAKE_BUILD_TYPE "Debug") # 设置构建类型为 Debug，可以进行gdb调试


# 输出路径
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin) # 可执行文件输出路径  ${项目根目录}/bin
set(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)    # 库文件输出路径 ${项目根目录}/lib


# 设置头文件搜索路径 （相当于 g++ 中的 -I 参数）
# 目前可能存在路径污染的问题，需要进一步解决
include_directories(${PROJECT_SOURCE_DIR}/src/common/include)
include_directories(${PROJECT_SOURCE_DIR}/src/fiber/include)
include_directories(${PROJECT_SOURCE_DIR}/src/rpc/include)
include_directories(${PROJECT_SOURCE_DIR}/example)
include_directories(${PROJECT_SOURCE_DIR}/src/raftCore/include)
include_directories(${PROJECT_SOURCE_DIR}/src/raftRpcPro/include)
include_directories(${PROJECT_SOURCE_DIR}/src/raftClerk/include)
include_directories(${PROJECT_SOURCE_DIR}/src/skipList/include)


# 设置库文件搜索路径 （相当于 g++ 中的 -L）
link_directories(${PROJECT_SOURCE_DIR}/lib)


# 添加子目录
add_subdirectory(src)
add_subdirectory(example)


# 自定义静态库目标：skip_list_on_raft
add_library(skip_list_on_raft STATIC  
            ${src_rpc} 
            ${src_fiber} 
            ${rpc_example} 
            ${raftsource} 
            ${src_raftCore} 
            ${src_raftRpcPro})
target_link_libraries(skip_list_on_raft muduo_net muduo_base pthread dl) # 为该库链接外部依赖库




# 添加格式化目标 start
# from : https://blog.csdn.net/guotianqing/article/details/121661067

# 添加自定义命令：格式化代码
add_custom_target(format
        COMMAND bash ${PROJECT_SOURCE_DIR}/format.sh    # 调用 bash format.sh 执行代码格式化脚本
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}         
        COMMAND echo "format done!"                     # 打印信息 "format done!"
)  # 运行方式：make format，常用于统一代码风格（如调用 clang-format）


# 添加格式化目标 end