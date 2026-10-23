# 15：同主机 SHM RPC

服务端和客户端在同一台 Linux 主机上，通过 POSIX 共享内存完成 RPC，不经过
UDP/TCP 协议栈，也不需要服务发现。

```bash
./example_15_shm_server
# 另一个终端
./example_15_shm_client
```

端点约定：

- 服务端：`shm://zrpc-example:1`
- 客户端：`shm://zrpc-example:2`
- 客户端 proxy 目标：`shm://zrpc-example:1`

限制：

- 第一版仅支持 Linux。
- 一个 channel 只支持一个 requester。
- 单条 encoded envelope 上限 64 KiB。
- 客户端必须先启动服务端，或服务端启动后客户端再启动。

实现位于 `src/transport/shm/`，公共注册接口见 `include/shm.h`。
