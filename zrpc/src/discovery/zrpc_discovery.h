#ifndef ZRPC_DISCOVERY_H
#define ZRPC_DISCOVERY_H

/*
 * mesh 服务发现（控制面）：UDP 广播 HELLO/BYE + 对端注册表。
 * 与数据面传输无关，只回答"谁提供哪个服务、端点在哪、走哪种传输"。
 * 它实现 zrpc_registry_backend_ops_t，把发现结果写入 node registry。
 */

#include <zrpc/zrpc_types.h>
#include <zrpc/zrpc_endpoint.h>

#include <stddef.h>
#include <stdint.h>

#include <ztk/ztk.h>

#include "zrpc_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zrpc_node zrpc_node_t;

/* 发现的 mesh 对端。 */
typedef struct zrpc_peer {
    int used;
    uint32_t ip;
    char ip_s[ZRPC_ENDPOINT_MAX];
    zrpc_endpoint_t endpoints[ZRPC_MAX_BINDINGS];
    size_t endpoint_count;
    uint32_t node_id;
    uint64_t last_seen_ms;
    int service_count;
    char services[ZRPC_PEER_MAX_SERVICES][ZRPC_SERVICE_NAME_MAX];
} zrpc_peer_t;

/* 显式发现目标（单播 HELLO）。 */
typedef struct zrpc_disc_target {
    int used;
    char ip[ZRPC_ENDPOINT_MAX];
    uint16_t port;
} zrpc_disc_target_t;

/* 发现子系统自有状态：由 node 内嵌，模块内聚访问。 */
typedef struct zrpc_discovery_state {
    ztk_socket *sock;
    zrpc_peer_t peers[ZRPC_MAX_PEERS];
    zrpc_disc_target_t targets[ZRPC_DISC_TARGET_MAX];
} zrpc_discovery_state_t;

int zrpc_discovery_init(zrpc_node_t *node);
void zrpc_discovery_shutdown(zrpc_node_t *node);
void zrpc_discovery_send_hello(zrpc_node_t *node, int bye);
void zrpc_discovery_on_readable(zrpc_node_t *node);
void zrpc_discovery_tick(zrpc_node_t *node, uint64_t now_ms);

int zrpc_discovery_find(zrpc_node_t *node, const char *service, char *ip_out, size_t ip_cap,
                        uint16_t *port_out, zrpc_transport_kind_t *transport_out);

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_DISCOVERY_H */
