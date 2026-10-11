#ifndef ZRPC_NODE_ACCESS_H
#define ZRPC_NODE_ACCESS_H

/*
 * node 访问门面。
 *
 * 供 node 作用域后端（如 discovery 控制面）以受控方式访问 node 状态，
 * 避免它们直接穿透 struct zrpc_node。实现于 zrpc_node_access.c。
 */

#include <zrpc/zrpc_types.h>

#include <stddef.h>
#include <stdint.h>

#include <ztk/ztk.h>

#include "zrpc_discovery.h"
#include "zrpc_registry.h"

uint32_t zrpc_node_id(const zrpc_node_t *node);
const char *zrpc_node_discovery_host(const zrpc_node_t *node);
uint32_t zrpc_node_ttl_ms(const zrpc_node_t *node);
ztk_poller *zrpc_node_poller(const zrpc_node_t *node);
ztk_poller_pool *zrpc_node_pool(const zrpc_node_t *node);
zrpc_registry_t *zrpc_node_registry(zrpc_node_t *node);
zrpc_discovery_state_t *zrpc_node_discovery(zrpc_node_t *node);
void zrpc_node_set_discovery_port(zrpc_node_t *node, uint16_t port);

/* 绑定/服务枚举（供发现帧构建）。 */
size_t zrpc_node_service_count(const zrpc_node_t *node);
const char *zrpc_node_service_name(const zrpc_node_t *node, size_t index);
const char *zrpc_node_binding_scheme(const zrpc_node_t *node, size_t index);
uint16_t zrpc_node_binding_port(const zrpc_node_t *node, size_t index);

#endif /* ZRPC_NODE_ACCESS_H */
