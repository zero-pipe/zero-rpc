#ifndef CONFIG_H
#define CONFIG_H

/*
 * network bridge 的单一配置入口。
 *
 * 配置按模块拆成独立子结构，node 只做装配；每个模块只关心自己的子配置。
 * 子结构全部在本头聚合，使用方只需 include <config.h>。
 *
 * {
 *   "name": "echo-server",
 *   "mode": "static",
 *   "runtime": {
 *     "io_threads": 2,
 *     "handler_threads": 4,
 *     "handler_max_concurrency": 64,
 *     "max_inflight": 65536,
 *     "max_streams": 64,
 *     "peer_rate_limit_pps": 0
 *   },
 *   "transport": {
 *     "kind": "tcp",
 *     "bind_host": "0.0.0.0",
 *     "data_port": 42420,
 *     "frag_bytes": 1200,
 *     "max_msg_bytes": 1048576,
 *     "backpressure_bytes": 4194304,
 *     "rtcp_interval_ms": 1000,
 *     "rtcp": true,
 *     "bindings": ["tcp://127.0.0.1:42420"]
 *   },
 *   "discovery": {
 *     "mode": "mesh",
 *     "broadcast": "255.255.255.255:41953",
 *     "hello_interval_ms": 3000,
 *     "node_ttl_ms": 9000,
 *     "peers": ["127.0.0.1:41953"]
 *   },
 *   "metrics": { "disabled": false, "latency_disabled": false, "sample_every": 1 },
 *   "log":     { "disabled": false, "level": "info" },
 *   "routes":  [ { "service": "EchoService", "endpoint": "tcp://127.0.0.1:42420" } ]
 * }
 *
 * 兼容：为平滑迁移，顶层扁平键（transport/bind_host/data_port/discovery_host/
 * discovery_port/io_threads/...）仍然接受，并在解析时映射进对应子结构。
 * 已知字段类型错误、取值越界或 URI 非法时返回 ERR_INVALID；未知字段忽略。
 *
 * config_t 拥有所有字符串；node_create / node_create_from_config 会把需要的
 * 字符串与端点拷进节点自身缓冲区，因此 config 只需活到建节点返回即可。
 */

#include <endpoint.h>
#include <service_table.h>
#include <types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 容量 ---- */
#define CONFIG_MAX_ROUTES 32
#define CONFIG_MAX_PEERS 32
#define CONFIG_MAX_BINDINGS MAX_BINDINGS

/* ---- 绑定 ---- */
typedef struct binding_config {
    const char *name;     /* provider/binding name, e.g. "tcp" */
    const char *endpoint; /* URI, e.g. tcp://127.0.0.1:8080 */
} binding_config_t;

/* ---- 分模块配置 ---- */

/* 线程与并发/流控策略。0 表示使用模块默认值。 */
typedef struct node_runtime_config {
    unsigned io_threads;              /* poller 线程数，0 = 硬件并发 */
    unsigned handler_threads;         /* worker 线程池，0 = 关闭 */
    unsigned handler_max_concurrency; /* worker 在途上限，0 = 不限 */
    uint32_t max_inflight;            /* 客户端 pending 上限，0 = 默认 65536 */
    uint32_t max_streams;             /* 并发接收流上限，0 = 默认 64 */
    uint32_t peer_rate_limit_pps;     /* 每对端每秒请求上限，0 = 不限 */
} node_runtime_config_t;

/* 传输/链路调优。 */
typedef struct node_transport_config {
    transport_kind_t transport; /* 默认 binding 的传输，默认 UDP */
    const char *bind_host;      /* 绑定地址，默认 "0.0.0.0" */
    uint16_t data_port;         /* 数据面端口，0 = 自动分配 */
    uint16_t frag_bytes;        /* UDP 每分片字节，0 = 默认 1200 */
    uint32_t max_msg_bytes;     /* 单条 envelope 上限，0 = 默认 1 MiB */
    uint32_t backpressure_bytes; /* 出站高水位，0 = 默认 4 MiB */
    uint32_t rtcp_interval_ms;   /* UDP RTCP 间隔，0 = 默认 1000ms */
    int rtcp_disabled;           /* 非 0 关闭 RTCP */
    const binding_config_t *bindings; /* 非空时启动所有 binding */
    size_t binding_count;
} node_transport_config_t;

/* 服务发现。 */
typedef struct node_discovery_config {
    int disabled;               /* 非 0 关闭发现（等价 mode=static） */
    const char *host;           /* 广播/发现地址，默认 255.255.255.255 */
    uint16_t port;              /* 发现端口，0 = DISCOVERY_PORT */
    uint32_t hello_interval_ms; /* HELLO 间隔，0 = 3000 */
    uint32_t node_ttl_ms;       /* 对端过期，0 = 9000 */
} node_discovery_config_t;

/* 指标采集。_disabled 为 0 表示开启（零初始化即默认开启）。 */
typedef struct node_metrics_config {
    int disabled;          /* 非 0 关闭全部指标采集 */
    int latency_disabled;  /* 非 0 关闭延迟直方图 */
    uint32_t sample_every; /* 0/1 = 全采；N = 每 N 次采一次 */
    int per_peer_disabled; /* 非 0 关闭每对端统计 */
} node_metrics_config_t;

/* 日志。disabled 为 0 表示开启。 */
typedef enum node_log_level {
    NODE_LOG_ASSERT = 0,
    NODE_LOG_ERROR = 1,
    NODE_LOG_WARN = 2,
    NODE_LOG_INFO = 3,
    NODE_LOG_DEBUG = 4,
    NODE_LOG_VERBOSE = 5
} node_log_level_t;

typedef struct node_log_config {
    int disabled;  /* 非 0 关闭日志 */
    uint8_t level; /* 见 node_log_level_t，默认 NODE_LOG_INFO */
    int sink;      /* 预留：console/file/syslog */
    const char *file;
} node_log_config_t;

/* 组合根配置：身份 + 各模块子配置。 */
typedef struct node_config {
    const char *name;           /* 节点名（日志/发现），可空 */
    node_mode_t mode;           /* MESH（默认）/ STATIC */
    const service_table_t *service_table; /* 可选共享声明表，静态生命周期 */
    node_runtime_config_t runtime;
    node_transport_config_t transport;
    node_discovery_config_t discovery;
    node_metrics_config_t metrics;
    node_log_config_t log;
} node_config_t;

/* ---- 解析结果 ---- */
typedef struct static_route {
    char service[SERVICE_NAME_MAX];
    char endpoint[ENDPOINT_MAX + SCHEME_MAX + 8];
} static_route_t;

typedef struct peer_endpoint {
    char ip[ENDPOINT_MAX];
    uint16_t port;
} peer_endpoint_t;

typedef struct binding_endpoint {
    char name[SCHEME_MAX];
    char endpoint[ENDPOINT_MAX + SCHEME_MAX + 8];
} binding_endpoint_t;

typedef struct config {
    node_config_t node; /* 指针指向下面的缓冲区 */
    char name[ZRI_NAME_MAX];
    char bind_host[ENDPOINT_MAX];
    char discovery_host[ENDPOINT_MAX];
    char log_file[ENDPOINT_MAX];
    static_route_t routes[CONFIG_MAX_ROUTES];
    size_t route_count;
    peer_endpoint_t peers[CONFIG_MAX_PEERS];
    size_t peer_count;
    binding_endpoint_t bindings[CONFIG_MAX_BINDINGS];
    binding_config_t binding_configs[CONFIG_MAX_BINDINGS];
    size_t binding_count;
} config_t;

/* ---- 默认值 ---- */
void node_runtime_config_default(node_runtime_config_t *cfg);
void node_transport_config_default(node_transport_config_t *cfg);
void node_discovery_config_default(node_discovery_config_t *cfg);
void node_metrics_config_default(node_metrics_config_t *cfg);
void node_log_config_default(node_log_config_t *cfg);
/* 填充组合根默认值；用于直接构造 node_config_t 的调用方。 */
void node_config_default(node_config_t *cfg);

/* ---- 加载与创建 ---- */
/* 解析 JSON 字符串；成功返回 OK 并填充 out。 */
int config_load(const char *json, config_t *out);
/* 读取文件后解析。 */
int config_load_file(const char *path, config_t *out);

/* 按配置创建节点，并应用静态路由与显式发现对端。 */
int node_create_from_config(const config_t *cfg, node_t **out);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
