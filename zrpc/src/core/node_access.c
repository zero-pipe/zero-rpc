/* node 访问门面的实现（供发现等 node 作用域后端使用）。 */
#include "node_access.h"

#include "node_internal.h"

uint32_t zri_node_id(const zrpc_node_t *node) {
    return node ? node->node_id : 0;
}

const char *zri_node_discovery_host(const zrpc_node_t *node) {
    return node ? node->discovery_host : NULL;
}

uint32_t zri_node_ttl_ms(const zrpc_node_t *node) {
    return node ? node->node_ttl_ms : 0;
}

ztk_poller *zri_node_poller(const zrpc_node_t *node) {
    return node ? node->poller : NULL;
}

ztk_poller_pool *zri_node_pool(const zrpc_node_t *node) {
    return node ? node->pool : NULL;
}

zri_registry_t *zri_node_registry(zrpc_node_t *node) {
    return node ? &node->registry : NULL;
}

zri_discovery_state_t *zri_node_discovery(zrpc_node_t *node) {
    return node ? &node->discovery : NULL;
}

void zri_node_set_discovery_port(zrpc_node_t *node, uint16_t port) {
    if (node) {
        node->discovery_port = port;
    }
}

size_t zri_node_service_count(const zrpc_node_t *node) {
    size_t n = 0;
    const zrpc_service_t *svc;
    if (!node) {
        return 0;
    }
    for (svc = node->services; svc; svc = svc->next) {
        n++;
    }
    return n;
}

const char *zri_node_service_name(const zrpc_node_t *node, size_t index) {
    const zrpc_service_t *svc;
    if (!node) {
        return NULL;
    }
    for (svc = node->services; svc; svc = svc->next) {
        if (index == 0) {
            return svc->name;
        }
        index--;
    }
    return NULL;
}

const char *zri_node_binding_scheme(const zrpc_node_t *node, size_t index) {
    if (!node || index >= node->transport_count || !node->transports[index]) {
        return NULL;
    }
    return node->transports[index]->scheme;
}

uint16_t zri_node_binding_port(const zrpc_node_t *node, size_t index) {
    if (!node || index >= node->transport_count || !node->transports[index]) {
        return 0;
    }
    return node->transports[index]->port;
}
