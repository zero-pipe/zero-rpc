#ifndef ZRPC_ROUTER_H
#define ZRPC_ROUTER_H

/*
 * 路由表：service -> 目标端点。
 *
 * 两条来源：
 *   - 静态路由：应用显式登记（zrpc_node_add_static_route）；
 *   - mesh 发现：由对端表重建（zrpc_route_rebuild）。
 *
 * 调用路径只做一次 O(1) 查表，不再扫描 peers×services。
 */

#include <zrpc/zrpc_types.h>
#include <zrpc/zrpc_endpoint.h>

#include <stddef.h>
#include <stdint.h>

#include <uthash.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zrpc_node zrpc_node_t;

typedef struct zrpc_route {
    char service[ZRPC_SERVICE_NAME_MAX];
    zrpc_endpoint_t endpoints[ZRPC_MAX_ENDPOINTS];
    size_t endpoint_count;
    size_t next_endpoint;
    int is_static; /* 静态路由：mesh 重建时不删除 */
    uint64_t gen;
    UT_hash_handle hh;
} zrpc_route_t;

void zrpc_route_init(zrpc_node_t *node);
void zrpc_route_cleanup(zrpc_node_t *node);

/* 由 mesh 对端表重建路由（发现变更后调用）。 */
void zrpc_route_rebuild(zrpc_node_t *node);

/* 显式设置一条静态路由（覆盖已有项）。 */
int zrpc_route_set(zrpc_node_t *node, const char *service, const char *ip, uint16_t port,
                   zrpc_transport_kind_t transport);
int zrpc_route_add_endpoint(zrpc_node_t *node, const char *service, const char *endpoint,
                            int is_static);

int zrpc_route_lookup(zrpc_node_t *node, const char *service, char *ip_out, size_t ip_cap,
                      uint16_t *port_out, zrpc_transport_kind_t *transport_out,
                      char *scheme_out, size_t scheme_cap);
/* Prefer a TCP endpoint when prefer_stream is set (large payload policy). */
int zrpc_route_lookup_prefer(zrpc_node_t *node, const char *service, int prefer_stream,
                             char *ip_out, size_t ip_cap, uint16_t *port_out,
                             zrpc_transport_kind_t *transport_out, char *scheme_out,
                             size_t scheme_cap);

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_ROUTER_H */
