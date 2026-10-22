#ifndef CONFIG_H
#define CONFIG_H

/*
 * 简单的 JSON 配置（基于内置 cJSON）。字段是扁平的，端点一律写成 URI：
 *
 * {
 *   "name": "echo-server",
 *   "mode": "static",
 *   "io_threads": 2,
 *   "bindings": ["tcp://127.0.0.1:42420"],
 *   "discovery": {
 *     "broadcast": "255.255.255.255:41953",
 *     "hello_interval_ms": 3000,
 *     "node_ttl_ms": 9000,
 *     "peers": ["127.0.0.1:41953"]
 *   },
 *   "routes": [
 *     { "service": "EchoService", "endpoint": "tcp://127.0.0.1:42420" }
 *   ]
 * }
 *
 * routes 是静态路由（service -> 目标端点），discovery 仅 mesh 模式使用。
 * 配置按严格 JSON 解析，不支持行注释或块注释。已知字段若类型错误、取值越界
 * 或端点/URI 非法，config_load 返回 ERR_INVALID；未知字段会被忽略。
 * mode 取值 "mesh"（默认）| "static"；transport 取值 "udp"（默认）| "tcp"。
 *
 * 可选的调优字段：transport（默认 binding 的传输）、bind_host、data_port、
 * frag_bytes、max_msg_bytes、backpressure_bytes、rtcp_interval_ms、rtcp、
 * handler_threads、handler_max_concurrency、max_streams、peer_rate_limit_pps。
 * "rtcp": false 关闭 RTCP 报告；rtcp_interval_ms 为 0 时用默认 1000ms。
 * handler_threads > 0 时创建 handler 线程池，配合 service_method_options_t
 * 的 SERVICE_EXEC_WORKER 把阻塞 handler 移出 poller；handler_max_concurrency
 * > 0 时限制未完成的 worker 任务数，超限请求回 ERR_AGAIN。
 * max_streams > 0 时限制单节点并发接收流数（默认 MAX_RECV_STREAMS）；
 * peer_rate_limit_pps > 0 时按对端 ip:port 做每秒请求限速，超限回 ERR_AGAIN。
 *
 * config_t 拥有所有字符串；node_create / node_create_from_config 会把需要的
 * 字符串与端点拷进节点自身缓冲区，因此 config 只需活到建节点返回即可，
 * 之后可安全释放。node_config_t 里的 bindings 指针同样只在 node_create
 * 期间被读取。
 */

#include <node.h>
#include <endpoint.h>
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
    static_route_t routes[CONFIG_MAX_ROUTES];
    size_t route_count;
    peer_endpoint_t peers[CONFIG_MAX_PEERS];
    size_t peer_count;
    binding_endpoint_t bindings[CONFIG_MAX_BINDINGS];
    binding_config_t binding_configs[CONFIG_MAX_BINDINGS];
    size_t binding_count;
} config_t;

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
