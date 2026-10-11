#ifndef ZRPC_ROUTER_H
#define ZRPC_ROUTER_H

/*
 * 路由表：service -> 目标端点。
 *
 * 当前来源为静态路由：应用显式登记（zrpc_node_add_static_route）。表自带锁，
 * 操作对象是 zrpc_route_table_t，不依赖 node；调用路径只做一次 O(1) 查表。
 */

#include <zrpc/zrpc_types.h>
#include <zrpc/zrpc_endpoint.h>

#include <stddef.h>
#include <stdint.h>

#include <uthash.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zrpc_route {
    char service[ZRPC_SERVICE_NAME_MAX];
    zrpc_endpoint_t endpoints[ZRPC_MAX_ENDPOINTS];
    size_t endpoint_count;
    size_t next_endpoint;
    int is_static; /* 预留：区分静态/动态来源 */
    UT_hash_handle hh;
} zrpc_route_t;

typedef struct zrpc_route_table {
    zrpc_route_t *routes;
    void *lock;
} zrpc_route_table_t;

void zrpc_route_init(zrpc_route_table_t *table);
void zrpc_route_cleanup(zrpc_route_table_t *table);

/* 显式设置一条静态路由（覆盖已有项）。 */
int zrpc_route_set(zrpc_route_table_t *table, const char *service, const char *ip, uint16_t port,
                   zrpc_transport_kind_t transport);
int zrpc_route_add_endpoint(zrpc_route_table_t *table, const char *service, const char *endpoint,
                            int is_static);

int zrpc_route_lookup(zrpc_route_table_t *table, const char *service, char *ip_out, size_t ip_cap,
                      uint16_t *port_out, zrpc_transport_kind_t *transport_out,
                      char *scheme_out, size_t scheme_cap);
/* Prefer a TCP endpoint when prefer_stream is set (large payload policy). */
int zrpc_route_lookup_prefer(zrpc_route_table_t *table, const char *service, int prefer_stream,
                             char *ip_out, size_t ip_cap, uint16_t *port_out,
                             zrpc_transport_kind_t *transport_out, char *scheme_out,
                             size_t scheme_cap);

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_ROUTER_H */
