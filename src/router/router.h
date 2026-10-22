#ifndef ROUTER_H
#define ROUTER_H

/*
 * 路由表：service -> 目标端点。
 *
 * 当前来源为静态路由：应用显式登记（nb_node_add_static_route）。表自带锁，
 * 操作对象是 nb_route_table_t，不依赖 node；调用路径只做一次 O(1) 查表。
 */

#include <types.h>
#include <endpoint.h>

#include <stddef.h>
#include <stdint.h>

#include <uthash.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nb_route {
    char service[NB_SERVICE_NAME_MAX];
    nb_endpoint_t endpoints[NB_MAX_ENDPOINTS];
    size_t endpoint_count;
    size_t next_endpoint;
    int is_static; /* 预留：区分静态/动态来源 */
    UT_hash_handle hh;
} nb_route_t;

typedef struct nb_route_table {
    nb_route_t *routes;
    void *lock;
} nb_route_table_t;

void nb_route_init(nb_route_table_t *table);
void nb_route_cleanup(nb_route_table_t *table);

/* 显式设置一条静态路由（覆盖已有项）。 */
int nb_route_set(nb_route_table_t *table,
                  const char *service,
                  const char *ip,
                  uint16_t port,
                  nb_transport_kind_t transport);
int nb_route_add_endpoint(nb_route_table_t *table,
                           const char *service,
                           const char *endpoint,
                           int is_static);

int nb_route_lookup(nb_route_table_t *table,
                     const char *service,
                     char *ip_out,
                     size_t ip_cap,
                     uint16_t *port_out,
                     nb_transport_kind_t *transport_out,
                     char *scheme_out,
                     size_t scheme_cap);
/* Prefer a TCP endpoint when prefer_stream is set (large payload policy). */
int nb_route_lookup_prefer(nb_route_table_t *table,
                            const char *service,
                            int prefer_stream,
                            char *ip_out,
                            size_t ip_cap,
                            uint16_t *port_out,
                            nb_transport_kind_t *transport_out,
                            char *scheme_out,
                            size_t scheme_cap);

#ifdef __cplusplus
}
#endif

#endif /* ROUTER_H */
