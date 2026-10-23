# network bridge framework

基于 [network-tool-kit](3rdparty/ntk) 的异步 RPC/消息运行时，内置 UDP 与 TCP 传输。
上层只面对 `node / service / proxy / payload`，传输和序列化都是可替换实现细节。

## 特性

- **三种调用形态**：`nb_proxy_call_async`、`nb_proxy_call_sync`、`nb_proxy_call_event`。
- **默认不新增线程**：回调在 NTK poller 线程运行；阻塞 handler 可选 WORKER 或自定义
  EXECUTOR。
- **可组合传输**：UDP 内置分片、NACK/RTX 与 RTCP 反馈；TCP 使用长度前缀分帧。
- **统一路由**：静态路由与 mesh 服务发现进入同一路由表。
- **payload 对框架透明**：框架只透传字节和 `encoding`，序列化通过 `nb_codec_t` 扩展。
- **流式 RPC**：支持客户端分片上传、服务端分片下载和双向流。
- **Cyclone DDS**：内置 `dds` provider，支持请求/响应与事件 pub/sub。
- **可观测性**：指标、延迟分位、日志运行期开关。
- **错误码体系**：稳定数值、名称、描述、可重试标记和应用错误保留区。

## 快速开始

```c
#include <nb.h>

static void on_response(int status, const nb_response_t *resp, void *user)
{
    (void)user;
    if (status == NB_OK) {
        /* resp->payload 仅在本次回调内有效 */
    }
}

int main(void)
{
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_proxy_t *proxy = NULL;
    nb_payload_t payload = { "hi", 2, 0 };

    nb_node_config_default(&cfg);
    cfg.name = "demo";
    cfg.mode = NB_MODE_STATIC;
    if (nb_node_create(&cfg, &node) != NB_OK) {
        return 1;
    }
    if (nb_proxy_create(node, NULL, &proxy) != NB_OK) {
        nb_node_destroy(node);
        return 1;
    }

    (void)nb_proxy_call_async(proxy,
                              "EchoService",
                              "echo",
                              &payload,
                              on_response,
                              NULL,
                              1000,
                              NULL);
    (void)nb_node_spin_once(node, 1000);

    nb_proxy_destroy(proxy);
    nb_node_destroy(node);
    return 0;
}
```

完整可运行代码见 [examples/](examples/)，索引见 [docs/examples.md](docs/examples.md)。

## 文档

| 文档 | 内容 |
| --- | --- |
| [SDK API](docs/api/sdk-api.md) | 对象模型、生命周期、线程与所有权 |
| [配置参考](docs/api/config.md) | JSON 字段、默认值与校验 |
| [错误码](docs/api/error-codes.md) | 稳定数值、名称、描述与重试语义 |
| [示例索引](docs/examples.md) | 按场景查找示例 |
| [测试与门禁](docs/testing.md) | unit / e2e / perf / stability / fuzz 的统一入口与判定 |
| [性能对比记录](docs/BENCH-STATUS.md) | TCP/UDP/DDS 远程对比与压测记录 |

入口头文件：

- 完整 API：`#include <nb.h>`
- 按角色最小引入：

| 头文件 | 适用场景 |
| --- | --- |
| `runtime.h` | 节点、调度、发现、路由、指标 |
| `server.h` | service、handler、reply、message view |
| `client.h` | proxy、stub、异步/同步/事件/流式调用 |
| `transport.h` | endpoint 与自定义 transport provider |
| `extensions.h` | codec、message view、service table |
| `config.h` | JSON 配置加载 |

## 目录

```text
include/     公共头文件
src/         内部实现（core / transport / message / router / registry / discovery / config / util）
examples/    按主题组织的可运行示例
docs/        API、配置、错误码和示例文档
tests/       unit / e2e 测试
tools/       统一门禁、压测与基准工具
3rdparty/    ntk、elog、cJSON、Cyclone DDS 等依赖
```

## 测试门禁

统一入口是 `tools/gate.py`，分类和 CTest label 对齐：

```bash
python3 tools/gate.py unit
python3 tools/gate.py e2e --transports tcp,udp,dds
python3 tools/gate.py perf --perf-duration 10
python3 tools/gate.py stability --transports tcp --stability-duration 60
python3 tools/gate.py all
```

详细判定标准、产物目录和新增测试约定见 [docs/testing.md](docs/testing.md)。

命名约定：库自有函数/类型/变量使用 `nb_`，宏/枚举常量使用 `NB_`。不透明句柄为
`struct nb_node` 等。

## 构建

Linux：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest --output-on-failure
```

Windows：

```powershell
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Debug
cd build
ctest -C Debug --output-on-failure
```

可选开关：

```text
BUILD_TESTS      构建测试（默认 ON）
BUILD_TOOLS      构建压测工具（默认 ON）
BUILD_EXAMPLES   构建示例（默认 ON）
BUILD_FUZZERS    构建 libFuzzer 目标（默认 OFF，需要 Clang）
ZERO_RPC_SANITIZE  address|undefined|thread|address,undefined
```

## 约定

- 服务/方法名用于路由，长度受 `NB_SERVICE_NAME_MAX` / `NB_METHOD_NAME_MAX` 限制。
- 单条消息默认上限 1 MiB，UDP 每分片默认 1200 字节。
- 请求/响应视图只在本次回调内有效；需要跨回调保存时请自行复制。
- 发送侧 payload 在调用返回前复制，调用方可在返回后立即释放。
- `nb_proxy_call_sync` 会阻塞，禁止在 poller 回调中使用。
- 同步调用写出的响应必须用 `nb_response_release` 释放。
- 框架错误码与应用状态码使用不同字段；应用错误码建议从 `NB_ERR_USER_BASE` 以下开始。

## 压测

性能与稳定性场景统一由 `tools/gate.py perf` / `tools/gate.py stability` 驱动，
底层压测引擎为 `tools/bench_server` + `tools/loadgen`。
