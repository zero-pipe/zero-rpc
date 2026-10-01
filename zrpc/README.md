# zrpc

基于 [zero-tool-kit](../../3rdpart/zero-tool-kit) 的**异步 RPC**，内置 **UDP** 与 **TCP**
传输，唯一底层依赖为 ZTK。整体分层解耦：上层只面对 `node / service / proxy / payload`，
传输与序列化都是可替换的实现细节。

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

## 目录

```
zrpc/
  include/zrpc/
    zrpc.h            伞头（包含全部）
    zrpc_types.h      状态码 / transport / payload 视图
    zrpc_node.h       节点：生命周期 / binding / 发现 / 静态路由
    zrpc_endpoint.h   endpoint URI 与 provider scheme
    zrpc_transport_provider.h 传输 provider 扩展 SPI
    zrpc_service.h    服务端：注册方法 / 处理请求 / reply
    zrpc_proxy.h      客户端：异步 call / cancel / endpoint
    zrpc_codec.h      payload codec 扩展点（预留）
    zrpc_config.h     JSON 配置
  src/
    core/             node / service / proxy / pending / codec
    router/           静态路由表
    registry/         service instance / endpoint snapshot / selector
    discovery/        registry backend（当前为 UDP mesh 广播）
    message/          provider-neutral RPC envelope 编解码
    transport/        传输 facade + provider SPI + UDP/TCP 驱动 + framing
      zrpc_rx_window  UDP 接收窗口与缺口检测
      zrpc_reasm      UDP 分片重组
      zrpc_tx_ring    UDP RTX 重传环
    config/           JSON 配置解析
  tests/              wire / loopback(udp+tcp) / config / registry
  tools/              压测 server / loadgen
  examples/           最小 echo server / client + 配置样例
```

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

完整可运行见 `examples/`：

```bash
./zrpc_echo_server examples/echo_server.json
./zrpc_echo_client examples/echo_client.json
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

```json
{
  "node": {
    "name": "srv",
    "transport": "udp",
    "bind_host": "0.0.0.0",
    "data_port": 0,
    "io_threads": 2,
    "mode": "mesh",
    "frag_bytes": 1200,
    "max_msg_bytes": 1048576
  },
  "discovery": {
    "host": "255.255.255.255",
    "port": 41953,
    "hello_interval_ms": 3000,
    "node_ttl_ms": 9000,
    "peers": [ { "ip": "127.0.0.1", "port": 41953 } ]
  },
  "routes": [
    { "service": "echo", "ip": "127.0.0.1", "port": 9000, "transport": "tcp" }
  ]
}
```

```c
zrpc_config_t cfg;
zrpc_config_load_file("config.json", &cfg);
zrpc_node_t *node;
zrpc_node_create_from_config(&cfg, &node); /* 创建节点并应用 peers / routes */
```

## 约定

- 服务/方法名用于路由（`service.method`），长度受限（`ZRPC_SERVICE_NAME_MAX` / `ZRPC_METHOD_NAME_MAX`）。
- `msg_id` 为 32 位；`request_id` 对外为 `uint64_t`，内部取低 32 位。
- 单条消息上限 1 MiB；UDP 每分片默认 1200 字节（收发两端须一致）。
- 请求/响应视图（含 payload）仅在本次回调内有效，需要跨回调保存时请自行拷贝。

## 压测

`tools/zrpc_bench_server` + `tools/zrpc_loadgen` + `tools/perf/zrpc_bench.py`，CLI/结果 JSON 与
fastcall-c 的 `rpc_bench` 契约对齐。用法见脚本头部说明。
