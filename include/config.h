#ifndef NB_CONFIG_H
#define NB_CONFIG_H

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
 * 字段类型错误、取值越界或 URI 非法时返回 NB_ERR_INVALID；未知字段忽略。
 * 需要定位失败字段时用 nb_config_load_ex，它额外返回字段路径与原因。
 *
 * nb_config_t 拥有所有字符串；nb_node_create / nb_node_create_from_config 会把需要的
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
#define NB_CONFIG_MAX_ROUTES 32
#define NB_CONFIG_MAX_PEERS 32
#define NB_CONFIG_MAX_BINDINGS NB_MAX_BINDINGS

/* ---- 绑定 ---- */
typedef struct nb_binding_config {
    const char *name;     /* provider/binding name, e.g. "tcp" */
    const char *endpoint; /* URI, e.g. tcp://127.0.0.1:8080 */
    nb_intra_process_t intra_process; /* provider-local event fast path */
} nb_binding_config_t;

/* ---- 分模块配置 ---- */

/* 线程与并发/流控策略。0 表示使用模块默认值。 */
typedef struct nb_node_runtime_config {
    unsigned io_threads;              /* poller 线程数，0 = 硬件并发 */
    unsigned handler_threads;         /* worker 线程池，0 = 关闭 */
    unsigned handler_max_concurrency; /* worker 在途上限，0 = 不限 */
    uint32_t max_inflight;            /* 客户端 pending 上限，0 = 默认 65536 */
    uint32_t max_streams;             /* 并发接收流上限，0 = 默认 64 */
    uint32_t peer_rate_limit_pps;     /* 每对端每秒请求上限，0 = 不限 */
} nb_node_runtime_config_t;

/* 传输/链路调优。 */
typedef struct nb_node_transport_config {
    nb_transport_kind_t transport; /* 默认 binding 的传输，默认 UDP */
    const char *bind_host;      /* 绑定地址，默认 "0.0.0.0" */
    uint16_t data_port;         /* 数据面端口，0 = 自动分配 */
    uint16_t frag_bytes;        /* UDP 每分片字节，0 = 默认 1200 */
    uint32_t max_msg_bytes;     /* 单条 envelope 上限，0 = 默认 1 MiB */
    uint32_t backpressure_bytes; /* 出站高水位，0 = 默认 4 MiB */
    uint32_t rtcp_interval_ms;   /* UDP RTCP 间隔，0 = 默认 1000ms */
    int rtcp_disabled;           /* 非 0 关闭 RTCP */
    const nb_binding_config_t *bindings; /* 非空时启动所有 binding */
    size_t binding_count;
} nb_node_transport_config_t;

/* 服务发现。 */
typedef struct nb_node_discovery_config {
    int disabled;               /* 非 0 关闭发现（等价 mode=static） */
    const char *host;           /* 广播/发现地址，默认 255.255.255.255 */
    uint16_t port;              /* 发现端口，0 = NB_DISCOVERY_PORT */
    uint32_t hello_interval_ms; /* HELLO 间隔，0 = 3000 */
    uint32_t node_ttl_ms;       /* 对端过期，0 = 9000 */
} nb_node_discovery_config_t;

/* 指标采集。_disabled 为 0 表示开启（零初始化即默认开启）。 */
typedef struct nb_node_metrics_config {
    int disabled;          /* 非 0 关闭全部指标采集 */
    int latency_disabled;  /* 非 0 关闭延迟直方图 */
    uint32_t sample_every; /* 0/1 = 全采；N = 每 N 次采一次 */
    int per_peer_disabled; /* 非 0 关闭每对端统计 */
} nb_node_metrics_config_t;

/* 日志。disabled 为 0 表示开启。 */
typedef enum nb_node_log_level {
    NB_NODE_LOG_ASSERT = 0,
    NB_NODE_LOG_ERROR = 1,
    NB_NODE_LOG_WARN = 2,
    NB_NODE_LOG_INFO = 3,
    NB_NODE_LOG_DEBUG = 4,
    NB_NODE_LOG_VERBOSE = 5
} nb_node_log_level_t;

typedef struct nb_node_log_config {
    int disabled;  /* 非 0 关闭日志 */
    uint8_t level; /* 见 nb_node_log_level_t，默认 NB_NODE_LOG_INFO */
    int sink;      /* 预留：console/file/syslog */
    const char *file;
} nb_node_log_config_t;

/* 组合根配置：身份 + 各模块子配置。 */
typedef struct nb_node_config {
    const char *name;           /* 节点名（日志/发现），可空 */
    nb_node_mode_t mode;           /* MESH（默认）/ STATIC */
    const nb_service_table_t *service_table; /* 可选共享声明表，静态生命周期 */
    nb_node_runtime_config_t runtime;
    nb_node_transport_config_t transport;
    nb_node_discovery_config_t discovery;
    nb_node_metrics_config_t metrics;
    nb_node_log_config_t log;
} nb_node_config_t;

/* ---- 解析结果 ---- */
typedef struct nb_static_route {
    char service[NB_SERVICE_NAME_MAX];
    char endpoint[NB_ENDPOINT_MAX + NB_SCHEME_MAX + 8];
} nb_static_route_t;

typedef struct nb_peer_endpoint {
    char ip[NB_ENDPOINT_MAX];
    uint16_t port;
} nb_peer_endpoint_t;

typedef struct nb_binding_endpoint {
    char name[NB_SCHEME_MAX];
    char endpoint[NB_ENDPOINT_MAX + NB_SCHEME_MAX + 8];
} nb_binding_endpoint_t;

typedef struct nb_config {
    nb_node_config_t node; /* 指针指向下面的缓冲区 */
    char name[NB_NAME_MAX];
    char bind_host[NB_ENDPOINT_MAX];
    char discovery_host[NB_ENDPOINT_MAX];
    char log_file[NB_ENDPOINT_MAX];
    nb_static_route_t routes[NB_CONFIG_MAX_ROUTES];
    size_t route_count;
    nb_peer_endpoint_t peers[NB_CONFIG_MAX_PEERS];
    size_t peer_count;
    nb_binding_endpoint_t bindings[NB_CONFIG_MAX_BINDINGS];
    nb_binding_config_t binding_configs[NB_CONFIG_MAX_BINDINGS];
    size_t binding_count;
} nb_config_t;

/* 结构化解析错误：path 形如 "transport.frag_bytes" / "routes[0].endpoint"。 */
typedef struct nb_config_error {
    int code;         /* ERR_* */
    char path[96];
    char reason[128];
} nb_config_error_t;

/* ---- 默认值 ---- */
void nb_node_runtime_config_default(nb_node_runtime_config_t *cfg);
void nb_node_transport_config_default(nb_node_transport_config_t *cfg);
void nb_node_discovery_config_default(nb_node_discovery_config_t *cfg);
void nb_node_metrics_config_default(nb_node_metrics_config_t *cfg);
void nb_node_log_config_default(nb_node_log_config_t *cfg);
/* 填充组合根默认值；用于直接构造 nb_node_config_t 的调用方。 */
void nb_node_config_default(nb_node_config_t *cfg);
/*
 * 归一化：把 0/NULL 字段补成默认值并夹到合法范围。nb_node_create 内部会对传入
 * 配置的副本调用一次，因此直接构造 nb_node_config_t 的调用方无需再写兜底。
 * log.level 的 0 是合法的 NB_NODE_LOG_ASSERT，不会被改写。
 */
void nb_node_config_normalize(nb_node_config_t *cfg);

/* ---- 加载与创建 ---- */
/* 解析 JSON 字符串；成功返回 NB_OK 并填充 out。 */
int nb_config_load(const char *json, nb_config_t *out);
/* 同 nb_config_load，额外输出失败字段路径与原因；err 可为 NULL。 */
int nb_config_load_ex(const char *json, nb_config_t *out, nb_config_error_t *err);
/* 读取文件后解析。 */
int nb_config_load_file(const char *path, nb_config_t *out);
/* 同 nb_config_load_file，额外输出失败字段路径与原因；err 可为 NULL。 */
int nb_config_load_file_ex(const char *path, nb_config_t *out, nb_config_error_t *err);

/* 按配置创建节点，并应用静态路由与显式发现对端。 */
int nb_node_create_from_config(const nb_config_t *cfg, nb_node_t **out);

#ifdef __cplusplus
}
#endif

#endif /* NB_CONFIG_H */
