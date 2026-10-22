#ifndef DISCOVERY_H
#define DISCOVERY_H

/*
 * mesh 服务发现（控制面）：UDP 广播 HELLO/BYE + 对端注册表。
 * 与数据面传输无关，只回答"谁提供哪个服务、端点在哪、走哪种传输"。
 * 它实现 nb_registry_backend_ops_t，把发现结果写入 node registry。
 */

#include <types.h>
#include <endpoint.h>

#include <stddef.h>
#include <stdint.h>

#include <ntk.h>

#include "caps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nb_node nb_node_t;

/* 发现的 mesh 对端。 */
typedef struct nb_peer {
    int used;
    uint32_t ip;
    char ip_s[NB_ENDPOINT_MAX];
    nb_endpoint_t endpoints[NB_MAX_BINDINGS];
    size_t endpoint_count;
    uint32_t node_id;
    uint64_t last_seen_ms;
    int service_count;
    char services[PEER_MAX_SERVICES][NB_SERVICE_NAME_MAX];
} nb_peer_t;

/* 显式发现目标（单播 HELLO）。 */
typedef struct nb_disc_target {
    int used;
    char ip[NB_ENDPOINT_MAX];
    uint16_t port;
} nb_disc_target_t;

/* 发现子系统自有状态：由 node 内嵌，模块内聚访问。 */
typedef struct nb_discovery_state {
    ntk_socket *sock;
    nb_peer_t peers[MAX_PEERS];
    nb_disc_target_t targets[DISC_TARGET_MAX];
} nb_discovery_state_t;

int nb_discovery_init(nb_node_t *node);
void nb_discovery_shutdown(nb_node_t *node);
void nb_discovery_send_hello(nb_node_t *node, int bye);
void nb_discovery_on_readable(nb_node_t *node);
void nb_discovery_tick(nb_node_t *node, uint64_t now_ms);

int nb_discovery_find(nb_node_t *node,
                       const char *service,
                       char *ip_out,
                       size_t ip_cap,
                       uint16_t *port_out,
                       nb_transport_kind_t *transport_out);

#ifdef __cplusplus
}
#endif

#endif /* DISCOVERY_H */
