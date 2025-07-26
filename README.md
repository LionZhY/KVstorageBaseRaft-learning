分布式数据库学习自用

参考原项目：https://github.com/youngyangyang04/KVstorageBaseRaft-cpp

# 编译启动
## 编译

~~~shell
cd KVstorageBaseRaft-learning // 进入项目目录
mkdir cmake-build-debug
cd cmake-build-debug
cmake ..
make
~~~

编译之后的可执行文件都在目录 `bin`

## 启动 rpc
rpcprovider    是 `rpcExample/callee/friendService.cpp` 构建的可执行文件
consumer 是 `rpcExample/caller/callerFriendService.cpp` 构建的可执行文件

**先启动 RPC ：**

> 启动 RPC 服务节点，注册服务 FriendService() 

~~~shell
cd bin
./rpcprovider
~~~


**执行 consumer 示例：**

> 展示客户端通过 RPC 框架远程调用服务端的 GetFriendsList 方法

运行即可，**注意先运行provider，再运行consumer**，原因很简单：需要先提供rpc服务，才能去调用。

> 拆分一个新的终端窗口执行consumer，保留刚才的RPC窗口不退出

~~~shell
cd bin
./consumer
~~~

**原来的ip都设置的 `127.0.1.1`，全部改成自己的 `172.25.99.6`** （服务器）



## 启动 raft 集群

执行 `./raftCoreRun` （是 `raftCoreExample/raftKvDB.cpp` 构建的可执行文件）

模拟启动多个分布式 KVServer（Raft节点） 与端口配置

> 注意也是在拆分新终端里执行，要开着刚才的rpc终端，要不然会连接失败。

~~~shell
cd bin
./raftCoreRun -n 3 -f test.conf
~~~

示例是 3 个raft节点，配置信息放入 test.conf。


## 模拟 Clerk 向集群发送请求

在启动raft集群之后启动`callerMain` （是 `raftCoreExample/caller.cpp` 构建的可执行文件）

创建Clerk，模拟客户端持续向集群发送 Put 和 Get 请求

> 注意：再拆分一个终端窗口，刚才的rpc窗口和raftServer窗口都要开着

~~~shell
cd bin
./callerMain
~~~
