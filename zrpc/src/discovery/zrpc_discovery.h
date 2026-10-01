#ifndef ZRPC_DISCOVERY_H
#define ZRPC_DISCOVERY_H

/*
 * mesh 服务发现（控制面）：UDP 广播 HELLO/BYE + 对端注册表。
 * 与数据面传输无关，只回答"谁提供哪个服务、端点在哪、走哪种传输"。
 * 它实现 zrpc_registry_backend_ops_t，把发现结果写入 node registry。
 */

#include <zrpc/zrpc_types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zrpc_node zrpc_node_t;

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
