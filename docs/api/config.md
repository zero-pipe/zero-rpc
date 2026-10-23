# 配置参考

network bridge framework 使用 JSON 配置。配置可以来自文件或内存字符串：

```c
nb_config_t cfg;
nb_config_error_t err;

int rc = nb_config_load_file_ex("server.json", &cfg, &err);
if (rc != NB_OK) {
    printf("path=%s reason=%s\n", err.path, err.reason);
}

rc = nb_node_create_from_config(&cfg, &node);
```

对应接口：

- `nb_config_load` / `nb_config_load_ex`
- `nb_config_load_file` / `nb_config_load_file_ex`
- `nb_node_create_from_config`

`nb_config_t` 拥有解析后的字符串。`nb_node_create_from_config` 会把节点需要的字段复制到
节点内部，因此配置对象只需活到该调用返回。未知字段会被忽略；已知字段类型错误、越界或
URI 非法时返回 `NB_ERR_INVALID`，`_ex` 版本会给出 `path` 和 `reason`。

完整 JSON Schema 见 [`docs/config.schema.json`](../config.schema.json)。

## 最小服务端

```json
{
  "name": "echo-server",
  "mode": "static",
  "transport": {
    "kind": "tcp",
    "bindings": ["tcp://127.0.0.1:42420"]
  },
  "runtime": {
    "io_threads": 2
  }
}
```

## 最小客户端

```json
{
  "name": "echo-client",
  "mode": "static",
  "routes": [
    { "service": "EchoService", "endpoint": "tcp://127.0.0.1:42420" }
  ]
}
```

## 顶层字段

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `name` | string | `"rpc"` | 节点名，最长 `NB_NAME_MAX - 1` |
| `mode` | string | `"mesh"` | `mesh` 或 `static` |
| `runtime` | object | 见下表 | 线程、并发与限流 |
| `transport` | object | 见下表 | 传输与监听端点 |
| `discovery` | object | 见下表 | 服务发现 |
| `metrics` | object | 见下表 | 指标采集 |
| `log` | object | 见下表 | 日志 |
| `routes` | array | 空 | 静态路由 |

## runtime

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | ---: | --- |
| `io_threads` | uint | 硬件并发数 | poller 线程数，上限 `IO_MAX`（8） |
| `handler_threads` | uint | `0` | WORKER handler 线程数，`0` 表示关闭 |
| `handler_max_concurrency` | uint | `0` | worker/executor 未完成任务上限，`0` 表示不限 |
| `max_inflight` | uint | `65536` | 客户端未完成调用上限 |
| `max_streams` | uint | `64` | 并发接收流上限，上限 `MAX_RECV_STREAMS` |
| `peer_rate_limit_pps` | uint | `0` | 每对端每秒请求上限，`0` 表示不限 |

未配置 `handler_threads` 时，WORKER 方法退化为 INLINE 并告警。`handler_max_concurrency`
超限时新请求返回 `NB_ERR_OVERLOAD` 或 `NB_ERR_AGAIN`（按调用路径）。

## transport

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `kind` | string | `"udp"` | 默认传输，`udp` 或 `tcp` |
| `bind_host` | string | `"0.0.0.0"` | 默认绑定地址 |
| `data_port` | uint | `0` | 默认数据端口，`0` 表示自动分配 |
| `bindings` | array | 空 | 监听端点列表，元素见下 |
| `frag_bytes` | uint | `1200` | UDP 每分片字节，范围 `256..8192` |
| `max_msg_bytes` | uint | `1048576` | 单条消息上限，最大 `64 MiB` |
| `backpressure_bytes` | uint | `4194304` | 出站高水位，`0` 使用默认值 |
| `rtcp_interval_ms` | uint | `1000` | UDP RTCP 上报周期 |
| `rtcp` | bool | `true` | `false` 关闭 RTCP |

`bindings` 元素支持两种写法：

```json
{
  "transport": {
    "bindings": [
      "tcp://127.0.0.1:42420",
      { "name": "udp", "endpoint": "udp://0.0.0.0:42421" }
    ]
  }
}
```

字符串形式会根据 URI scheme 选择 UDP/TCP。对象形式的 `name` 是 binding 名称，省略时
使用 scheme。端口 `0` 表示由系统分配临时端口。

`kind`、`bind_host`、`data_port` 适用于没有显式 `bindings` 的默认 binding。

## discovery

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `mode` | string | `"mesh"` | `mesh` 开启发现；`static` / `off` / `none` 关闭 |
| `broadcast` | string | `"255.255.255.255:41953"` | 广播/发现地址，格式 `host:port` |
| `peers` | array | 空 | 额外单播 HELLO 目标，元素为 `"host:port"` |
| `hello_interval_ms` | uint | `3000` | HELLO 周期 |
| `node_ttl_ms` | uint | `9000` | 对端过期时间 |

顶层 `mode = "static"` 也会关闭服务发现。MESH 模式通常需要两端使用同一个发现端口或
互相配置单播 `peers`。

## metrics

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `disabled` | bool | `false` | 关闭全部指标采集 |
| `latency_disabled` | bool | `false` | 只关闭延迟直方图 |
| `sample_every` | uint | `1` | `1` 全采；`N` 每 N 次采样一次 |
| `per_peer_disabled` | bool | `false` | 关闭每对端链路统计 |

关闭指标后热路径只保留一次全局开关判断，不做原子累加。

## log

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `disabled` | bool | `false` | 关闭日志输出 |
| `level` | string | `"info"` | `assert` / `error` / `warn` / `info` / `debug` / `verbose` |
| `sink` | string | 空 | 预留字段，当前不改变输出后端 |
| `file` | string | 空 | 预留文件路径，最长 `NB_ENDPOINT_MAX - 1` |

运行期也可以用 `nb_log_configure(enabled, level)` 修改开关和级别。

## routes

```json
{
  "routes": [
    { "service": "OrderService", "endpoint": "tcp://127.0.0.1:42420" },
    { "service": "EchoService", "endpoint": "udp://127.0.0.1:42421" }
  ]
}
```

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | :---: | --- |
| `service` | string | 是 | 目标服务名 |
| `endpoint` | string | 是 | `scheme://host:port` |

静态路由跳过服务发现。最多 `NB_CONFIG_MAX_ROUTES` 条；超过上限返回 `NB_ERR_INVALID`。

## 默认值与归一化

通过代码直接构造 `nb_node_config_t` 时，先调用 `nb_node_config_default`，或在创建节点前
调用 `nb_node_config_normalize`。`nb_node_create` 会对配置副本做一次归一化。

归一化规则：

- `name == NULL` 使用 `"rpc"`。
- `transport.bind_host == NULL` 使用 `"0.0.0.0"`。
- `runtime.io_threads == 0` 使用硬件并发数，并夹到 `1..IO_MAX`。
- `frag_bytes` 夹到 `256..8192`。
- `max_msg_bytes` 夹到 `frag_bytes..64 MiB`。
- `backpressure_bytes == 0` 使用 `4 MiB`。
- `discovery.port == 0` 使用 `41953`。
- `metrics.sample_every == 0` 使用 `1`。

## 常见错误

| 错误路径 | 原因 |
| --- | --- |
| `runtime.io_threads` | 超过 `NB_MAX_IO_THREADS` |
| `transport.bindings` | 不是数组，元素不是 string/object，URI 非法或数量过多 |
| `transport.kind` | 不是 `udp` / `tcp` |
| `discovery.broadcast` | 不是 `host:port` |
| `routes[i].endpoint` | URI 非法 |
| `log.level` | 不是支持的级别 |
