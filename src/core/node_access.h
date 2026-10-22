#ifndef NODE_ACCESS_H
#define NODE_ACCESS_H

/*
 * node 访问门面。
 *
 * 供 node 作用域后端（如 discovery 控制面）以受控方式访问 node 状态，
 * 避免它们直接穿透 struct nb_node。实现于 nb_node_access.c。
 */

#include <types.h>

#include <stddef.h>
#include <stdint.h>

#include <ntk.h>

#include "discovery.h"
#include "registry.h"

uint32_t nb_node_id(const nb_node_t *node);
const char *nb_node_discovery_host(const nb_node_t *node);
uint32_t nb_node_ttl_ms(const nb_node_t *node);
ntk_poller *nb_node_poller(const nb_node_t *node);
ntk_poller_pool *nb_node_pool(const nb_node_t *node);
nb_registry_t *nb_node_registry(nb_node_t *node);
nb_discovery_state_t *nb_node_discovery(nb_node_t *node);
void nb_node_set_discovery_port(nb_node_t *node, uint16_t port);

/* 绑定/服务枚举（供发现帧构建）。 */
size_t nb_node_service_count(const nb_node_t *node);
const char *nb_node_service_name(const nb_node_t *node, size_t index);
const char *nb_node_binding_scheme(const nb_node_t *node, size_t index);
uint16_t nb_node_binding_port(const nb_node_t *node, size_t index);

#endif /* NODE_ACCESS_H */
