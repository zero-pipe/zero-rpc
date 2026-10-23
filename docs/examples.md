# 示例索引

示例按学习顺序排列，每个目录自包含源码、`CMakeLists.txt` 和 `README.md`。

| 目录 | 场景 | 运行方式 |
| --- | --- | --- |
| [01_hello_node](../examples/01_hello_node/README.md) | 创建/销毁 node | 单进程 |
| [02_service](../examples/02_service/README.md) | service 与同步 handler | 服务端 |
| [03_proxy](../examples/03_proxy/README.md) | proxy 生命周期 | 单进程 |
| [04_service_table](../examples/04_service_table/README.md) | 共享服务声明表 | 单进程 |
| [05_sync_call](../examples/05_sync_call/README.md) | 同步调用 02_service | 客户端 |
| [06_static_route](../examples/06_static_route/README.md) | 静态路由 | server + client |
| [07_direct_endpoint](../examples/07_direct_endpoint/README.md) | proxy 直连端点 | server + client |
| [08_config](../examples/08_config/README.md) | JSON 配置创建节点 | server + client |
| [09_service_discovery](../examples/09_service_discovery/README.md) | mesh 服务发现 | server + client |
| [10_payload_codec](../examples/10_payload_codec/README.md) | 应用 codec 往返 | 单进程 |
| [11_streaming](../examples/11_streaming/README.md) | 分片上传与流式回复 | 单进程 |
| [12_worker](../examples/12_worker/README.md) | WORKER handler 线程池 | 单进程 |
| [13_metadata_cancel](../examples/13_metadata_cancel/README.md) | metadata/deadline/cancel | 单进程 |
| [14_dds_pubsub](../examples/14_dds_pubsub/README.md) | Cyclone DDS 事件 pub/sub | 单进程 |

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Windows：

```powershell
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Release
```

示例可执行文件输出到构建目录的 `examples/` 下。`08_config` 的两个配置文件会自动复制到
可执行文件旁。

## 阅读顺序

先看 01、02、03 理解 node/service/proxy，再看 04/05 理解声明契约和调用，
然后看 06/07 理解路由，最后看 08/09 和 10-14 理解配置、发现、codec、流、
并发语义与 DDS pub/sub。
