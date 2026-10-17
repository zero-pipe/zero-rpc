#ifndef CONFIG_H
#define CONFIG_H

/*
 * 简单的 JSON 配置（基于内置 cJSON）。字段是扁平的，端点一律写成 URI：
 *
 * {
 *   "name": "echo-server",
 *   "mode": "static",                    // "mesh"（默认）| "static"
 *   "io_threads": 2,
 *   "bindings": ["tcp://127.0.0.1:42420"],   // 本节点监听的端点
 *   "discovery": {                       // mesh 模式才用到
 *     "broadcast": "255.255.255.255:41953",
 *     "hello_interval_ms": 3000,
 *     "node_ttl_ms": 9000,
 *     "peers": ["127.0.0.1:41953"]       // 额外的单播 HELLO 目标
 *   },
 *   "routes": [                          // 静态路由：service -> 目标端点
 *     { "service": "EchoService", "endpoint": "tcp://127.0.0.1:42420" }
 *   ]
 * }
 *
 * 可选的调优字段：transport（默认 binding 的传输）、bind_host、data_port、
 * frag_bytes、max_msg_bytes、backpressure_bytes、rtcp_interval_ms、rtcp。
 * "rtcp": false 关闭 RTCP 报告；rtcp_interval_ms 为 0 时用默认 1000ms。
 *
 * 兼容旧格式：字段仍可写在 "node" 对象里，binding 仍可写成
 * transport.bindings，peer 仍可写成 {ip,port}，route 仍可写成 ip/port/transport。
 * 两种写法同时存在时，扁平顶层字段优先。
 *
 * config_t 拥有所有字符串；node.* 指针指向自身缓冲区，因此 config 必须
 * 比由它创建的节点活得更久。
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
    char ip[ENDPOINT_MAX];
    char endpoint[ENDPOINT_MAX + SCHEME_MAX + 8];
    uint16_t port;
    transport_kind_t transport;
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
    char name[NAME_MAX];
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
