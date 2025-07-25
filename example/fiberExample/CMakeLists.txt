
# 定义一个名为 LIB_LIB 的变量，用于集中管理多个需要链接的库
set(
    LIB_LIB
    fiber_lib   # 项目中构建协程相关的静态库
    pthread     # POSIX 线程库，协程底层需要与系统线程协作
    dl          # Linux 动态链接库，fiber_lib 内部使用了 dlopen/dlsym 等函数
)



# 创建名为 test_server 的可执行文件，入口为 server.cpp
add_executable(test_server server.cpp)
# test_server 链接 LIB_LIB 中列出的所有库
target_link_libraries(test_server ${LIB_LIB})
#add_dependencies(test_server monsoon)


# 构建名为 test_scheduler 的可执行文件
add_executable(test_scheduler test_scheduler.cpp)
target_link_libraries(test_scheduler ${LIB_LIB})
#add_dependencies(test_scheduler monsoon)


# 构建名为 test_iomanager 的可执行文件
add_executable(test_iomanager test_iomanager.cpp)
target_link_libraries(test_iomanager ${LIB_LIB})
#add_dependencies(test_iomanager monsoon)


# 构建名为 test_hook 的可执行文件
add_executable(test_hook test_hook.cpp)
target_link_libraries(test_hook ${LIB_LIB})
#add_dependencies(test_hook monsoon)