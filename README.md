# zero-rpc

基于 [network-tool-kit](3rdpart/ntk) 的**异步核心 RPC**，内置 **UDP** 与 **TCP**
传输，唯一底层依赖为 NTK。整体分层解耦：上层只面对 `node / service / proxy / payload`，
传输与序列化都是可替换的实现细节。

接口抽象对齐 fastcall-c：`node` 管生命周期与传输，`stub` 承载传输无关的调用原语，
`proxy` 在 `stub` 之上注入路由；`service` 声明/处理方法；`service_table` 让两端共享一份
声明契约。详见下文「分层抽象」。

## 特性

- **三种调用形态**：`proxy_call_async(...)` 异步回调、`proxy_call_sync(...)` 阻塞等待、
  `proxy_call_event(...)` 单向事件。核心仍是异步的，sync 只是可选的阻塞封装。
- **不新增线程**：全部运行在 NTK 的 `ntk_poller_pool` 线程上；回调都在 poller 线程触发（禁止阻塞）。
- **可组合传输**：node 管理一个或多个 binding；每个 binding 用 `scheme://host:port` 标识。
  上层只看到统一的 envelope + 消息上下文。UDP 在内部用 RTP/RTCP 承载分片、NACK/RTX 与
  链路反馈来实现可靠性；TCP 本身可靠，只加长度前缀分帧，不引入 RTP/RTCP。
- **路由**：静态路由（显式登记 `service -> endpoint`）与 **mesh 服务发现**（UDP 广播 HELLO/BYE）统一进路由表。
- **payload 对框架透明**：框架只透传字节 + `encoding` 标签；序列化/反序列化通过可插拔 `codec_t` 留给应用层。
- **简单 JSON 配置**：内置 cJSON，`config_load[_file]` 一行加载。
- **流式 RPC（Phase 1）**：`service_add_stream_method` + `proxy_stream_*` 支持
  客户端分片上传；`reply_stream` + `proxy_call_stream` 支持服务端分片下载；
  UDP/TCP 共用同一 envelope。
- **分层 stub**：`stub_t` 是 `node` 之上、传输无关的调用原语（请求/事件 + 关联 +
  重传 + 可插拔 send），`proxy_t` 构建在其之上。
- **共享声明表**：`SERVICE_DEFINE` 在两端共享一个头文件，install 后即可按名查 ID。
- **同步风格 handler**：`service_add_sync_method` 让 handler 直接返回状态码并写响应
  view，框架异步回复。
- **同步便捷层**：`proxy_call_sync` / `response_release`（可选的阻塞封装，核心
  仍是异步）。

## 目录

```
zero-rpc/
  include/            公共头文件（角色入口 + 细粒度头）
    runtime.h    运行时入口：node / metrics
    server.h     服务端入口：service / reply / message view
    client.h     客户端入口：proxy / stub / async / sync / stream
    transport.h  传输扩展入口：endpoint / provider
    extensions.h 扩展入口：codec / message view / service table
    config.h     JSON 配置入口
    types.h      状态码 / transport / payload 视图
    node.h       节点的细粒度接口
    service.h    服务端的细粒度接口
    proxy.h      客户端 proxy 的细粒度接口
    stub.h       客户端 stub 的细粒度接口
    endpoint.h / transport_provider.h
    codec.h / message_view.h / service_table.h
    metrics.h    运行指标细粒度接口
  src/                内部实现（文件用短名，目录已表达归属）
    core/             组合根与内部子模块（按文件/职责切分）
      node.c              节点生命周期 + 传输装配 + 阻塞运行
      dispatch.c          入站消息分发 + 出站传输选择
      service.c           方法注册 + 请求/分片分发
      reply.c             服务端回复编码
      proxy.c             代理生命周期 + 路由解析 + 普通调用
      stream.c            客户端流式会话
      stub.c              传输无关调用原语
      pending.c           pending 关联表
      sync.c              同步便捷层
      message_view.c / service_table.c / codec.c / mem.c / util.c
      内部头按模块拆分（无聚合“上帝头”，各模块只 include 自己需要的）：
        caps.h               容量常量
        pending.h            pending 表
        service_internal.h   服务端内部结构
        proxy_internal.h     客户端内部结构（proxy/stub/stream）
        node_internal.h      节点（组合根）
    router/           静态路由表（自带锁，独立于 node，操作 zri_route_table_t）
    registry/         service instance / endpoint snapshot / selector（自带锁，独立于 node）
    discovery/        registry backend（UDP mesh 广播）；node 作用域的控制面后端
                      自持对端表/发现 socket/单播目标（node->discovery）
                      只经 node_access.h 门面访问 node，不穿透 struct node
    message/          provider-neutral RPC envelope 编解码 + 统一消息上下文
      message.h          zri_message_t：kind/status/request_id/route/payload/stream/peer
      envelope          编解码，以及 envelope -> zri_message_t 的转换
    transport/        传输 facade + provider SPI + UDP/TCP 驱动 + framing
      只读 zri_transport_t 上的配置快照，不穿透 node 内部
      framing         UDP 的 RTP/分片/NACK、RTCP SR/RR/SDES 与 TCP 长度前缀分帧
                      （均为传输内部实现，不进入上层消息抽象）
      rx_window       UDP 接收窗口与缺口检测
      reasm           UDP 分片重组
      tx_ring         UDP RTX 重传环
    config/           JSON 配置解析

  命名约定：公开 API 不带命名空间前缀（见 include/）；库内部符号一律 `zri_`
  （对齐 nng 的 `nng_`/`nni_` 双命名空间）。
  tests/              framing / loopback(udp+tcp) / config / registry
  tools/              压测 server / loadgen
  examples/           编号示例（对齐 fastcall-c：node / service / proxy / stub / table / 路由）
  3rdpart/ntk         network-tool-kit（唯一底层依赖）
```

## 公共 API 分层

新代码按使用角色选择入口头文件；这些入口只负责组合现有细粒度头，不改变已有类型和函数名。

| 入口 | 适用场景 | 主要内容 |
| --- | --- | --- |
| `runtime.h` | 所有程序 | 节点生命周期、调度、发现、路由、指标 |
| `server.h` | 提供 RPC 服务 | service、handler、reply、消息视图 |
| `client.h` | 调用 RPC 服务 | proxy、stub、异步/同步/事件/流式调用 |
| `transport.h` | 自定义传输或端点处理 | endpoint URI、provider SPI |
| `extensions.h` | 接入业务序列化或共享契约 | codec、消息视图、service table |
| `config.h` | 使用 JSON 配置 | 配置加载和按配置创建节点 |

例如，服务端通常只需要：

```c
#include <server.h>
#include <config.h>  /* 使用 JSON 配置时再引入 */
```

客户端通常只需要：

```c
#include <client.h>
```

细粒度的 `node.h`、`proxy.h`、`service.h` 等头文件继续保留，适合库封装层
或需要最小 include 集合的项目。

## 分层抽象

```
node            生命周期 / binding / 发现 / 静态路由
  ├─ service    注册方法；handler 可异步 reply，或用同步风格直接返回
  ├─ stub       service.method + payload -> 关联(request_id) + 重传 + send 钩子
  └─ proxy      在 stub 上注入端点覆盖 / 路由；提供 call_async / call_sync / call_event
```

- **node 的角色**：一个 node 可以只做客户端（创建 proxy）、只做服务端（创建 service），
  也可以两者兼具。proxy 借用 node 的 poller/路由，不持有独立线程。
- **统一消息上下文**：传输层与 core 之间只传一个 `zri_message_t`（kind/status/request_id/
  route/payload/stream/peer），不再用一长串位置参数穿过 dispatch / service / reply。
- **stub**：不注入 send 时走节点默认路由；`proxy_create` 会自动创建一个注入了
  `proxy` send 钩子的 stub，因此 `proxy_call_async` 与 `stub_call_async` 共享同一条
  发送/重传路径。
- **service_table**：把 service/method 及数字 ID 声明在一个共享头文件里，两端 install
  同一张表；`node_config_t::service_table` 会在节点创建时自动 install。
- **同步 handler**：`sync_handler_fn(req, resp, user)` 返回值即应用状态码，响应写进
  `message_view_t`；框架在回调返回后释放视图并异步回复。

示例（`examples/`，编号对齐 fastcall-c）：

- `01_hello_node` 节点生命周期。
- `02_service` / `05_sync_call` 同步 handler 服务端 + 同步调用客户端。
- `03_proxy` proxy 生命周期。
- `04_service_table` 共享声明契约。
- `06_static_route_{server,client}` 静态路由。
- `07_direct_endpoint_{server,client}` 直连端点。
- `08_config_{server,client}` 从 JSON 配置创建节点（`bindings` / `routes` 用 URI）。

## 构建

Linux：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest --output-on-failure   # CMake < 3.20 用这种方式；>= 3.20 可用 ctest --test-dir build
```

Windows：

```powershell
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Debug
cd build
ctest -C Debug --output-on-failure
```

## 最小用法

```c
#include <client.h>
#include <server.h>

static void handler(call_t *call, const request_t *req, void *user) {
    reply(call, 0, &req->payload); /* 异步回显，payload 原样返回 */
}

static void on_resp(int status, const response_t *resp, void *user) {
    if (status == OK) {
        /* resp->payload.data / len / encoding */
    }
}

/* 服务端 */
node_config_t cfg = {0};
cfg.name = "srv";
cfg.transport = TRANSPORT_UDP; /* 或 TRANSPORT_TCP */
cfg.discovery_port = 41953;
node_t *node = NULL;
node_create(&cfg, &node);
service_t *svc = NULL;
service_create(node, "echo", &svc);
service_add_method(svc, "ping", handler, NULL);

/* 客户端 */
proxy_t *proxy = NULL;
proxy_create(node, NULL, &proxy);
payload_t pl = { "hi", 2, 0 };
proxy_call_async(proxy, "echo", "ping", &pl, on_resp, NULL, 1000, NULL);
```

完整可运行见 `examples/`（编号对齐 fastcall-c）：

```bash
./example_01_hello_node
./example_02_service             # 服务端 TCP 127.0.0.1:42410
./example_05_sync_call           # 客户端（先启动 02）
./example_06_static_route_server &
./example_06_static_route_client
./example_08_config_server &       # 读取 08_config_server.json
./example_08_config_client         # 读取 08_config_client.json
```

## payload 与 codec（序列化扩展点）

框架不解释 payload。业务对象与字节流的转换由应用通过 `codec_t` 完成：

```c
static const codec_t my_codec = {
    "json", 1, my_encode, my_decode, my_release, NULL,
};
node_register_codec(node, &my_codec);

void *bytes; size_t len;
my_codec.encode(NULL, &order, &bytes, &len);
payload_t pl = { bytes, len, my_codec.encoding };
proxy_call_async(proxy, "OrderService", "get", &pl, on_resp, NULL, 1000, NULL);
```

`encoding` 随消息透传，接收端用 `node_find_codec(node, resp->payload.encoding)` 找到对应 codec 解码。

## JSON 配置

配置是扁平的，端点统一写成 URI，大多数场景一眼就能看懂：

```json
{
  "name": "echo-server",
  "mode": "static",
  "io_threads": 2,
  "bindings": ["tcp://127.0.0.1:42420"],
  "routes": [
    { "service": "EchoService", "endpoint": "tcp://127.0.0.1:42420" }
  ]
}
```

- `bindings`：本节点监听的端点列表，元素是 `scheme://host:port`（端口 0 表示自动分配）。
- `routes`：静态路由，`service` 指向目标 `endpoint`，跳过服务发现。
- mesh 模式再加 `discovery` 段：

```json
{
  "name": "echo-server",
  "mode": "mesh",
  "bindings": ["udp://0.0.0.0:42420"],
  "discovery": {
    "broadcast": "255.255.255.255:41953",
    "hello_interval_ms": 3000,
    "node_ttl_ms": 9000,
    "peers": ["127.0.0.1:41953"]
  }
}
```

- `discovery.broadcast` 是 `host:port`，`peers` 是额外的单播 HELLO 目标，元素为 `host:port`。
- 可选调优字段（顶层）：`transport`、`bind_host`、`data_port`、`frag_bytes`、`max_msg_bytes`、`backpressure_bytes`、`rtcp_interval_ms`（RTCP SR/RR 上报间隔，默认 1000ms）、`rtcp`（`false` 关闭 RTCP）。
- 兼容旧格式：字段可放在 `node` 对象里，binding 可用 `transport.bindings`，peer 可用 `{ip,port}`，route 可用 `ip/port/transport`；同时存在时扁平顶层字段优先。

完整可运行见 `examples/08_config_{server,client}.json`。

```c
config_t cfg;
config_load_file("config.json", &cfg);
node_t *node;
node_create_from_config(&cfg, &node); /* 创建节点并应用 peers / routes */
```

注意：`config_t` 拥有所有字符串，节点只借用其指针，因此 `cfg` 必须比节点活得更久。

## 约定

- 服务/方法名用于路由（`service.method`），长度受限（`SERVICE_NAME_MAX` / `METHOD_NAME_MAX`）。
- `msg_id` 为 32 位；`request_id` 对外为 `uint64_t`，内部取低 32 位。
- 单条消息上限 1 MiB；UDP 每分片默认 1200 字节（收发两端须一致）。
- 请求/响应视图（含 payload）仅在本次回调内有效，需要跨回调保存时请自行拷贝。

## 压测

`tools/bench_server` + `tools/loadgen` + `tools/perf/zbench.py`，CLI/结果 JSON 与
fastcall-c 的 `rpc_bench` 契约对齐。用法见脚本头部说明。
