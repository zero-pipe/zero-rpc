/* node 访问门面的实现（供发现等 node 作用域后端使用）。 */
#include "node_access.h"

#include "node_internal.h"

uint32_t nb_node_id(const nb_node_t *node)
{
    return node ? node->node_id : 0;
}

const char *nb_node_discovery_host(const nb_node_t *node)
{
    return node ? node->discovery_host : NULL;
}

uint32_t nb_node_ttl_ms(const nb_node_t *node)
{
    return node ? node->node_ttl_ms : 0;
}

ntk_poller *nb_node_poller(const nb_node_t *node)
{
    return node ? node->poller : NULL;
}

ntk_poller_pool *nb_node_pool(const nb_node_t *node)
{
    return node ? node->pool : NULL;
}

nb_registry_t *nb_node_registry(nb_node_t *node)
{
    return node ? &node->registry : NULL;
}

nb_discovery_state_t *nb_node_discovery(nb_node_t *node)
{
    return node ? &node->discovery : NULL;
}

void nb_node_set_discovery_port(nb_node_t *node, uint16_t port)
{
    if (node) {
        node->discovery_port = port;
    }
}

size_t nb_node_service_count(const nb_node_t *node)
{
    size_t n = 0;
    const nb_service_t *svc;
    if (!node) {
        return 0;
    }
    if (node->services_lock) {
        ntk_mutex_lock((ntk_mutex *)node->services_lock);
    }
    for (svc = node->services; svc; svc = svc->next) {
        if (!svc->removed) {
            n++;
        }
    }
    if (node->services_lock) {
        ntk_mutex_unlock((ntk_mutex *)node->services_lock);
    }
    return n;
}

const char *nb_node_service_name(const nb_node_t *node, size_t index)
{
    const nb_service_t *svc;
    if (!node) {
        return NULL;
    }
    if (node->services_lock) {
        ntk_mutex_lock((ntk_mutex *)node->services_lock);
    }
    for (svc = node->services; svc; svc = svc->next) {
        if (svc->removed) {
            continue;
        }
        if (index == 0) {
            if (node->services_lock) {
                ntk_mutex_unlock((ntk_mutex *)node->services_lock);
            }
            return svc->name;
        }
        index--;
    }
    if (node->services_lock) {
        ntk_mutex_unlock((ntk_mutex *)node->services_lock);
    }
    return NULL;
}

const char *nb_node_binding_scheme(const nb_node_t *node, size_t index)
{
    if (!node || index >= node->transport_count || !node->transports[index]) {
        return NULL;
    }
    return node->transports[index]->scheme;
}

uint16_t nb_node_binding_port(const nb_node_t *node, size_t index)
{
    if (!node || index >= node->transport_count || !node->transports[index]) {
        return 0;
    }
    return node->transports[index]->port;
}
