# zrpc

基于 [zero-tool-kit](../../3rdpart/zero-tool-kit) 的**异步 RPC**，内置 **UDP** 与 **TCP**
传输，唯一底层依赖为 ZTK。整体分层解耦：上层只面对 `node / service / proxy / payload`，
传输与序列化都是可替换的实现细节。

接口抽象对齐 fastcall-c：`node` 管生命周期与传输，`stub` 承载传输无关的调用原语，
`proxy` 在 `stub` 之上注入路由；`service` 声明/处理方法；`service_table` 让两端共享一份
声明契约。详见下文「分层抽象」。

## 特性

- **只提供异步接口**：`zrpc_proxy_call(...)` 发送后立即返回，结果通过回调送达；无阻塞/同步等待 API。
- **不新增线程**：全部运行在 ZTK 的 `ztk_poller_pool` 线程上；回调都在 poller 线程触发（禁止阻塞）。
- **可组合传输**：node 管理一个或多个 binding；每个 binding 用 `scheme://host:port` 标识。
  `UDP` 走 RTP + 分片/重组 + NACK/RTX；`TCP` 走基于 `ztk_tcp_server/ztk_tcp_client` 的长度前缀分帧。
- **路由**：静态路由（显式登记 `service -> endpoint`）与 **mesh 服务发现**（UDP 广播 HELLO/BYE）统一进路由表。
- **payload 对框架透明**：框架只透传字节 + `encoding` 标签；序列化/反序列化通过可插拔 `zrpc_codec_t` 留给应用层。
- **简单 JSON 配置**：内置 cJSON，`zrpc_config_load[_file]` 一行加载。
- **流式 RPC（Phase 1）**：`zrpc_service_add_stream_method` + `zrpc_proxy_stream_*` 支持
  客户端分片上传；`zrpc_reply_stream` + `zrpc_proxy_call_stream` 支持服务端分片下载；
  UDP/TCP 共用同一 envelope。
- **分层 stub**：`zrpc_stub_t` 是 `node` 之上、传输无关的调用原语（请求/事件 + 关联 +
  重传 + 可插拔 send），`zrpc_proxy_t` 构建在其之上。
- **共享声明表**：`ZRPC_SERVICE_DEFINE` 在两端共享一个头文件，install 后即可按名查 ID。
- **同步风格 handler**：`zrpc_service_add_sync_method` 让 handler 直接返回状态码并写响应
  view，框架异步回复。
- **同步便捷层**：`zrpc_proxy_call_sync` / `zrpc_response_release`（可选的阻塞封装，核心
  仍是异步）。

## 目录

```
zrpc/
  include/zrpc/
    zrpc.h            伞头（包含全部）
     zrpc_types.h      状态码 / transport / payload 视图
     zrpc_metrics.h    节点运行指标
     zrpc_node.h       节点：生命周期 / binding / 发现 / 静态路由
    zrpc_endpoint.h   endpoint URI 与 provider scheme
    zrpc_transport_provider.h 传输 provider 扩展 SPI
    zrpc_service.h    服务端：注册方法 / 处理请求 / reply
    zrpc_proxy.h      客户端：异步 call / cancel / endpoint
    zrpc_codec.h      payload codec 扩展点（预留）
    zrpc_message_view.h 请求/响应统一视图（同步 handler 用）
    zrpc_service_table.h 服务/方法声明契约（ZRPC_SERVICE_DEFINE）
    zrpc_stub.h       node 之上的传输无关调用原语
    zrpc_sync.h       同步调用便捷层（可选）
    zrpc_config.h     JSON 配置
  src/
    core/             组合根与内部子模块（按文件/职责切分）
      zrpc_node.c         节点生命周期 + 传输装配 + 阻塞运行
      zrpc_dispatch.c     入站消息分发 + 出站传输选择
      zrpc_service.c      方法注册 + 请求/分片分发
      zrpc_reply.c        服务端回复编码
      zrpc_proxy.c        代理生命周期 + 路由解析 + 普通调用
      zrpc_stream.c       客户端流式会话
      zrpc_stub.c         传输无关调用原语
      zrpc_pending.c      pending 关联表
      zrpc_sync.c         同步便捷层
      zrpc_message_view.c / zrpc_service_table.c / zrpc_codec.c / zrpc_mem.c
      内部头按模块拆分（无聚合“上帝头”，各模块只 include 自己需要的）：
        zrpc_caps.h            容量常量
        zrpc_pending.h         pending 表
        zrpc_service_internal.h 服务端内部结构
        zrpc_proxy_internal.h  客户端内部结构（proxy/stub/stream）
        zrpc_node_internal.h   节点（组合根）
    router/           静态路由表（自带锁，独立于 node，操作 zrpc_route_table_t）
    registry/         service instance / endpoint snapshot / selector（自带锁，独立于 node）
    discovery/        registry backend（UDP mesh 广播）；node 作用域的控制面后端
                      自持对端表/发现 socket/单播目标（node->discovery）
                      只经 zrpc_node_access.h 门面访问 node，不穿透 struct zrpc_node
    message/          provider-neutral RPC envelope 编解码
    transport/        传输 facade + provider SPI + UDP/TCP 驱动 + framing
      只读 zrpc_transport_t 上的配置快照，不穿透 node 内部
      zrpc_framing    RTP/分片/NACK 与 TCP 长度前缀分帧
      zrpc_rx_window  UDP 接收窗口与缺口检测
      zrpc_reasm      UDP 分片重组
      zrpc_tx_ring    UDP RTX 重传环
    config/           JSON 配置解析
  tests/              framing / loopback(udp+tcp) / config / registry
  tools/              压测 server / loadgen
  examples/           编号示例（对齐 fastcall-c：node / service / proxy / stub / table / 路由）
```

## 分层抽象

```
node            生命周期 / binding / 发现 / 静态路由
  └─ stub       service.method + payload -> 关联(request_id) + 重传 + send 钩子
       └─ proxy 在 stub 上注入端点覆盖 / 路由
service         注册方法；handler 可异步 reply，或用同步风格直接返回
```

- **stub**：不注入 send 时走节点默认路由；`zrpc_proxy_create` 会自动创建一个注入了
  `proxy` send 钩子的 stub，因此 `zrpc_proxy_call` 与 `zrpc_stub_call_async` 共享同一条
  发送/重传路径。
- **service_table**：把 service/method 及数字 ID 声明在一个共享头文件里，两端 install
  同一张表；`zrpc_node_config_t::service_table` 会在节点创建时自动 install。
- **同步 handler**：`zrpc_sync_handler_fn(req, resp, user)` 返回值即应用状态码，响应写进
  `zrpc_message_view_t`；框架在回调返回后释放视图并异步回复。

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
#include <zrpc/zrpc.h>

static void handler(zrpc_call_t *call, const zrpc_request_t *req, void *user) {
    zrpc_reply(call, 0, &req->payload); /* 异步回显，payload 原样返回 */
}

static void on_resp(int status, const zrpc_response_t *resp, void *user) {
    if (status == ZRPC_OK) {
        /* resp->payload.data / len / encoding */
    }
}

/* 服务端 */
zrpc_node_config_t cfg = {0};
cfg.name = "srv";
cfg.transport = ZRPC_TRANSPORT_UDP; /* 或 ZRPC_TRANSPORT_TCP */
cfg.discovery_port = 41953;
zrpc_node_t *node = NULL;
zrpc_node_create(&cfg, &node);
zrpc_service_t *svc = NULL;
zrpc_service_create(node, "echo", &svc);
zrpc_service_add_method(svc, "ping", handler, NULL);

/* 客户端 */
zrpc_proxy_t *proxy = NULL;
zrpc_proxy_create(node, NULL, &proxy);
zrpc_payload_t pl = { "hi", 2, 0 };
zrpc_proxy_call(proxy, "echo", "ping", &pl, on_resp, NULL, 1000, NULL);
```

完整可运行见 `examples/`（编号对齐 fastcall-c）：

```bash
./zrpc_example_01_hello_node
./zrpc_example_02_service             # 服务端 TCP 127.0.0.1:42410
./zrpc_example_05_sync_call           # 客户端（先启动 02）
./zrpc_example_06_static_route_server &
./zrpc_example_06_static_route_client
./zrpc_example_08_config_server &       # 读取 08_config_server.json
./zrpc_example_08_config_client         # 读取 08_config_client.json
```

## payload 与 codec（序列化扩展点）

框架不解释 payload。业务对象与字节流的转换由应用通过 `zrpc_codec_t` 完成：

```c
static const zrpc_codec_t my_codec = {
    "json", 1, my_encode, my_decode, my_release, NULL,
};
zrpc_node_register_codec(node, &my_codec);

void *bytes; size_t len;
my_codec.encode(NULL, &order, &bytes, &len);
zrpc_payload_t pl = { bytes, len, my_codec.encoding };
zrpc_proxy_call(proxy, "OrderService", "get", &pl, on_resp, NULL, 1000, NULL);
```

`encoding` 随消息透传，接收端用 `zrpc_node_find_codec(node, resp->payload.encoding)` 找到对应 codec 解码。

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
- 可选调优字段（顶层）：`transport`、`bind_host`、`data_port`、`frag_bytes`、`max_msg_bytes`、`backpressure_bytes`。
- 兼容旧格式：字段可放在 `node` 对象里，binding 可用 `transport.bindings`，peer 可用 `{ip,port}`，route 可用 `ip/port/transport`；同时存在时扁平顶层字段优先。

完整可运行见 `examples/08_config_{server,client}.json`。

```c
zrpc_config_t cfg;
zrpc_config_load_file("config.json", &cfg);
zrpc_node_t *node;
zrpc_node_create_from_config(&cfg, &node); /* 创建节点并应用 peers / routes */
```

注意：`zrpc_config_t` 拥有所有字符串，节点只借用其指针，因此 `cfg` 必须比节点活得更久。

## 约定

- 服务/方法名用于路由（`service.method`），长度受限（`ZRPC_SERVICE_NAME_MAX` / `ZRPC_METHOD_NAME_MAX`）。
- `msg_id` 为 32 位；`request_id` 对外为 `uint64_t`，内部取低 32 位。
- 单条消息上限 1 MiB；UDP 每分片默认 1200 字节（收发两端须一致）。
- 请求/响应视图（含 payload）仅在本次回调内有效，需要跨回调保存时请自行拷贝。

## 压测

`tools/zrpc_bench_server` + `tools/zrpc_loadgen` + `tools/perf/zrpc_bench.py`，CLI/结果 JSON 与
fastcall-c 的 `rpc_bench` 契约对齐。用法见脚本头部说明。
