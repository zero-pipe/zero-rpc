#ifndef ZRPC_CONFIG_H
#define ZRPC_CONFIG_H

/*
 * 简单的 JSON 配置（基于内置 cJSON）。
 *
 * 配置文件示例：
 * {
 *   "node": {
 *     "name": "srv",
 *     "transport": "udp",            // "udp" | "tcp"
 *     "bind_host": "0.0.0.0",
 *     "data_port": 0,
 *     "io_threads": 2,
 *     "mode": "mesh",                // "mesh" | "static"
 *     "frag_bytes": 1200
 *   },
 *   "discovery": {
 *     "host": "255.255.255.255",
 *     "port": 41953,
 *     "hello_interval_ms": 3000,
 *     "node_ttl_ms": 9000,
 *     "peers": [ { "ip": "127.0.0.1", "port": 41953 } ]
 *   },
 *   "routes": [
 *     { "service": "echo", "ip": "127.0.0.1", "port": 9000, "transport": "tcp" }
 *   ]
 * }
 *
 * zrpc_config_t 拥有所有字符串；node.* 指针指向自身缓冲区，因此 config 必须
 * 比由它创建的节点活得更久。
 */

#include <zrpc/zrpc_node.h>
#include <zrpc/zrpc_endpoint.h>
#include <zrpc/zrpc_types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZRPC_CONFIG_MAX_ROUTES 32
#define ZRPC_CONFIG_MAX_PEERS 32
#define ZRPC_CONFIG_MAX_BINDINGS ZRPC_MAX_BINDINGS

typedef struct zrpc_static_route {
    char service[ZRPC_SERVICE_NAME_MAX];
    char ip[ZRPC_ENDPOINT_MAX];
    char endpoint[ZRPC_ENDPOINT_MAX + ZRPC_SCHEME_MAX + 8];
    uint16_t port;
    zrpc_transport_kind_t transport;
} zrpc_static_route_t;

typedef struct zrpc_peer_endpoint {
    char ip[ZRPC_ENDPOINT_MAX];
    uint16_t port;
} zrpc_peer_endpoint_t;

typedef struct zrpc_binding_endpoint {
    char name[ZRPC_SCHEME_MAX];
    char endpoint[ZRPC_ENDPOINT_MAX + ZRPC_SCHEME_MAX + 8];
} zrpc_binding_endpoint_t;

typedef struct zrpc_config {
    zrpc_node_config_t node; /* 指针指向下面的缓冲区 */
    char name[ZRPC_NAME_MAX];
    char bind_host[ZRPC_ENDPOINT_MAX];
    char discovery_host[ZRPC_ENDPOINT_MAX];
    zrpc_static_route_t routes[ZRPC_CONFIG_MAX_ROUTES];
    size_t route_count;
    zrpc_peer_endpoint_t peers[ZRPC_CONFIG_MAX_PEERS];
    size_t peer_count;
    zrpc_binding_endpoint_t bindings[ZRPC_CONFIG_MAX_BINDINGS];
    zrpc_binding_config_t binding_configs[ZRPC_CONFIG_MAX_BINDINGS];
    size_t binding_count;
} zrpc_config_t;

/* 解析 JSON 字符串；成功返回 ZRPC_OK 并填充 out。 */
int zrpc_config_load(const char *json, zrpc_config_t *out);
/* 读取文件后解析。 */
int zrpc_config_load_file(const char *path, zrpc_config_t *out);

/* 按配置创建节点，并应用静态路由与显式发现对端。 */
int zrpc_node_create_from_config(const zrpc_config_t *cfg, zrpc_node_t **out);

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_CONFIG_H */
