#ifndef DISCOVERY_H
#define DISCOVERY_H

/*
 * mesh 服务发现（控制面）：UDP 广播 HELLO/BYE + 对端注册表。
 * 与数据面传输无关，只回答"谁提供哪个服务、端点在哪、走哪种传输"。
 * 它实现 zri_registry_backend_ops_t，把发现结果写入 node registry。
 */

#include <types.h>
#include <endpoint.h>

#include <stddef.h>
#include <stdint.h>

#include <ztk/ztk.h>

#include "caps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct node node_t;

/* 发现的 mesh 对端。 */
typedef struct zri_peer {
    int used;
    uint32_t ip;
    char ip_s[ENDPOINT_MAX];
    endpoint_t endpoints[MAX_BINDINGS];
    size_t endpoint_count;
    uint32_t node_id;
    uint64_t last_seen_ms;
    int service_count;
    char services[PEER_MAX_SERVICES][SERVICE_NAME_MAX];
} zri_peer_t;

/* 显式发现目标（单播 HELLO）。 */
typedef struct zri_disc_target {
    int used;
    char ip[ENDPOINT_MAX];
    uint16_t port;
} zri_disc_target_t;

/* 发现子系统自有状态：由 node 内嵌，模块内聚访问。 */
typedef struct zri_discovery_state {
    ztk_socket *sock;
    zri_peer_t peers[MAX_PEERS];
    zri_disc_target_t targets[DISC_TARGET_MAX];
} zri_discovery_state_t;

int zri_discovery_init(node_t *node);
void zri_discovery_shutdown(node_t *node);
void zri_discovery_send_hello(node_t *node, int bye);
void zri_discovery_on_readable(node_t *node);
void zri_discovery_tick(node_t *node, uint64_t now_ms);

int zri_discovery_find(node_t *node, const char *service, char *ip_out, size_t ip_cap,
                        uint16_t *port_out, transport_kind_t *transport_out);

#ifdef __cplusplus
}
#endif

#endif /* DISCOVERY_H */
