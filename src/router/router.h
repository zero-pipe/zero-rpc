#ifndef ROUTER_H
#define ROUTER_H

/*
 * 路由表：service -> 目标端点。
 *
 * 当前来源为静态路由：应用显式登记（node_add_static_route）。表自带锁，
 * 操作对象是 zri_route_table_t，不依赖 node；调用路径只做一次 O(1) 查表。
 */

#include <types.h>
#include <endpoint.h>

#include <stddef.h>
#include <stdint.h>

#include <uthash.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zri_route {
    char service[SERVICE_NAME_MAX];
    endpoint_t endpoints[MAX_ENDPOINTS];
    size_t endpoint_count;
    size_t next_endpoint;
    int is_static; /* 预留：区分静态/动态来源 */
    UT_hash_handle hh;
} zri_route_t;

typedef struct zri_route_table {
    zri_route_t *routes;
    void *lock;
} zri_route_table_t;

void zri_route_init(zri_route_table_t *table);
void zri_route_cleanup(zri_route_table_t *table);

/* 显式设置一条静态路由（覆盖已有项）。 */
int zri_route_set(zri_route_table_t *table,
                  const char *service,
                  const char *ip,
                  uint16_t port,
                  transport_kind_t transport);
int zri_route_add_endpoint(zri_route_table_t *table,
                           const char *service,
                           const char *endpoint,
                           int is_static);

int zri_route_lookup(zri_route_table_t *table,
                     const char *service,
                     char *ip_out,
                     size_t ip_cap,
                     uint16_t *port_out,
                     transport_kind_t *transport_out,
                     char *scheme_out,
                     size_t scheme_cap);
/* Prefer a TCP endpoint when prefer_stream is set (large payload policy). */
int zri_route_lookup_prefer(zri_route_table_t *table,
                            const char *service,
                            int prefer_stream,
                            char *ip_out,
                            size_t ip_cap,
                            uint16_t *port_out,
                            transport_kind_t *transport_out,
                            char *scheme_out,
                            size_t scheme_cap);

#ifdef __cplusplus
}
#endif

#endif /* ROUTER_H */
