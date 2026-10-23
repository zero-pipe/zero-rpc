# network bridge framework SDK API

network bridge framework 是异步 RPC/消息运行时。公共 API 只暴露节点、服务、代理和字节视图，
payload 的序列化格式由应用通过 codec 决定。

## 入口头文件

需要完整 API 时：

```c
#include <nb.h>
```

需要最小依赖集合时，按角色选择：

| 头文件 | 用途 |
| --- | --- |
| `runtime.h` | 节点、调度、发现、路由、指标 |
| `server.h` | service、handler、回复、消息视图 |
| `client.h` | proxy、stub、异步/同步/事件/流式调用 |
| `transport.h` | endpoint 与自定义 transport provider |
| `extensions.h` | codec、message view、service table |
| `config.h` | JSON 配置加载与按配置创建节点 |

`error.h`、`types.h`、`metrics.h`、`log.h` 由上述入口按需包含。

## 对象模型

```text
node
  ├─ service   注册方法，接收请求并回复
  ├─ stub      service.method + payload，维护 request_id 与重传
  └─ proxy     在 stub 上注入服务发现/路由，提供 call_async/call_sync/call_event
```

一个 node 可以同时承载客户端和服务端。proxy 借用 node 的 poller 与路由，不创建独立线程。

推荐创建与销毁顺序：

```text
nb_node_create
  -> nb_service_create / nb_proxy_create
  -> nb_node_spin 或业务事件循环
  -> nb_service_destroy / nb_proxy_destroy
  -> nb_node_destroy
```

## 线程模型

- 默认 callback 和 `NB_SERVICE_EXEC_INLINE` handler 在 NTK poller 线程运行，禁止阻塞。
- `NB_SERVICE_EXEC_WORKER` 在节点的 `runtime.handler_threads` 线程池运行。
- `NB_SERVICE_EXEC_EXECUTOR` 在调用者提供的执行器运行。
- `nb_proxy_call_async` / `nb_proxy_call_event` 可从任意线程调用。
- `nb_proxy_call_sync` 会阻塞，禁止在 poller 回调中调用。
- worker/executor handler 的 `nb_reply` 会自动回投原 poller 发送。

## 生命周期与所有权

### Payload

`nb_payload_t` 只是 `{data, len, encoding}` 视图。框架不解释内容。

发送侧 `nb_proxy_call_async`、`nb_stub_call_async`、`nb_proxy_call_event` 会在返回前复制
payload。调用方始终拥有自己的缓冲区，可以在调用返回后立即释放。

接收侧请求/响应视图只在本次回调内有效。需要跨回调保存时必须复制。

### 同步调用

```c
nb_response_t response = {0};
int rc = nb_proxy_call_sync(proxy, "OrderService", "get", &request, &response, 3000, NULL);
/* 使用 response.payload */
nb_response_release(&response);
```

`nb_response_release` 释放同步调用写出的响应 metadata 与 payload，并将结构清零。

### 消息视图

`nb_message_view_t` 用于同步风格 handler 和业务侧组装 payload：

```c
nb_message_view_t view;
nb_message_view_init(&view);
nb_message_view_set_string(&view, "ok");
nb_message_view_release(&view);
```

`set_borrowed` 不接管外部内存；`set_data` / `set_string` / `set_payload` 复制并由视图拥有。

## Node

```c
int nb_node_create(const nb_node_config_t *cfg, nb_node_t **out);
void nb_node_destroy(nb_node_t *node);
```

节点创建时完成传输、poller、发现和路由装配。查询接口：

- `nb_node_name`
- `nb_node_transport`
- `nb_node_data_port`
- `nb_node_discovery_port`
- `nb_node_binding_count` / `nb_node_binding_endpoint`
- `nb_node_metrics` / `nb_node_peer_stats`

运行与路由：

- `nb_node_add_discovery_peer`
- `nb_node_add_static_route` / `nb_node_add_static_endpoint`
- `nb_node_wait_for_service`
- `nb_node_post` / `nb_node_post_delay`
- `nb_node_spin_once` / `nb_node_spin`

配置字段见 [配置参考](config.md)。

## Service

异步 handler：

```c
typedef void (*handler_fn)(nb_call_t *call, const nb_request_t *req, void *user);
```

同步风格 handler：

```c
typedef int (*sync_handler_fn)(const nb_request_t *req, nb_message_view_t *resp, void *user);
```

同步 handler 的返回值会作为应用状态码写回响应。异步 handler 必须调用一次 `nb_reply`、
`nb_reply_ex` 或 `nb_reply_stream`。

注册：

```c
nb_service_t *svc = NULL;
nb_service_create(node, "OrderService", &svc);
nb_service_add_method(svc, "get", on_get, user);
nb_service_add_sync_method(svc, "put", on_put_sync, user);
```

`*_ex` 版本通过 `nb_service_method_options_t` 选择 INLINE、WORKER 或 EXECUTOR。
WORKER 需要 `runtime.handler_threads > 0`；未配置时退化为 INLINE 并告警。

流式：

- `nb_service_add_stream_method` / `_ex`
- `nb_reply_stream`
- 请求 chunk 的最后一个必须带 `NB_STREAM_LAST`

拦截与取消：

- `nb_service_set_interceptor` 在 handler 前执行，非 `NB_OK` 直接短路回复。
- `nb_call_deadline_ms` 读取客户端传播的绝对 deadline。
- `nb_call_canceled` 支持 handler 协作式退出。

## Proxy 与 Stub

普通调用：

```c
nb_proxy_t *proxy = NULL;
nb_proxy_create(node, NULL, &proxy);

nb_payload_t request = { "hello", 5, 0 };
nb_proxy_call_async(proxy, "EchoService", "echo", &request, on_response, user, 1000, NULL);
```

三种形态：

- `nb_proxy_call_async`：等待响应，回调一次。
- `nb_proxy_call_sync`：阻塞等待并写出 `nb_response_t`。
- `nb_proxy_call_event`：单向事件，不等待响应。

带 metadata 的变体为 `_ex` 版本。metadata 使用固定容量容器，二进制 value 可安全传输。

流式调用：

- `nb_proxy_call_stream`：普通请求，响应分片。
- `nb_proxy_stream_open` / `nb_proxy_stream_open_bidi`：打开客户端请求流。
- `nb_proxy_stream_send` / `nb_proxy_stream_cancel` / `nb_proxy_stream_destroy`。

取消：

```c
uint64_t request_id = 0;
nb_proxy_call_async(..., &request_id);
nb_proxy_cancel(proxy, request_id);
```

取消是 best-effort：框架发送 `KIND_CANCEL` 并让本地调用以 `NB_ERR_NOTFOUND` 结束；
已经运行的 handler 不会被抢占。

`nb_stub_t` 是 proxy 下方的调用原语。`nb_proxy_create` 会自动创建一个注入路由 send 钩子的
stub。只有需要自定义发送逻辑时才直接使用 `nb_stub_create` 与 `nb_stub_call_*`。

## Codec

```c
typedef struct nb_codec {
    const char *name;
    uint8_t encoding;
    int (*encode)(void *ctx, const void *obj, void **out_data, size_t *out_len);
    int (*decode)(void *ctx, const void *data, size_t len, void **out_obj);
    void (*release)(void *ctx, void *obj);
    void *ctx;
} nb_codec_t;
```

注册：

```c
nb_node_register_codec(node, &my_codec);
const nb_codec_t *codec = nb_node_find_codec(node, response.payload.encoding);
```

encode 产生的字节在发送调用返回后即可 release，因为框架已经复制 payload。

## Service Table

把服务和方法 ID 放在两端共享的头文件里：

```c
NB_SERVICE_DEFINE(order_table, 7u, "OrderService",
    NB_SERVICE_METHOD(1u, "get"),
    NB_SERVICE_METHOD(2u, "put"));
```

然后把 `&order_table` 填入 `nb_node_config_t.service_table`，或在创建节点前调用
`nb_install_service_table`。服务表是声明契约，路由仍使用 `service.method` 字符串。

## Transport Provider

内置 UDP/TCP 是明文、无认证传输。需要 TLS/DTLS 或自定义协议时实现
`nb_transport_provider_ops_t`，并在创建节点前注册：

```c
nb_transport_provider_register(&my_provider);
```

provider 的 descriptor 和 ops 必须具有静态生命周期。

## 指标与日志

```c
nb_metrics_t metrics;
nb_node_metrics(node, &metrics);

const nb_metric_desc_t *desc = nb_metric_find("requests_sent");
```

指标可通过 `node.metrics.disabled` 关闭；关闭后热路径只保留一次全局标志判断。

日志：

```c
#define NB_LOG_TAG "app"
#include <log.h>

nb_log_info("started node=%s", nb_node_name(node));
```

`nb_log_configure(0, level)` 可运行期关闭全部日志输出。

## 错误处理

每个 `create`、`start`、`call` 都应检查返回值。错误码的稳定名称、描述、可重试标记见
[错误码](error-codes.md)。

## 示例

完整可运行代码见 [示例索引](../examples.md)。
