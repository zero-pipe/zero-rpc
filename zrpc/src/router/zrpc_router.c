/* 路由表：service -> 目标端点（静态路由 + mesh 发现）。 */
#include "zrpc_router.h"

#include "zrpc_internal.h"

#include <stdlib.h>
#include <string.h>

void zrpc_route_init(zrpc_node_t *node) {
    if (!node) {
        return;
    }
    node->routes = NULL;
    node->route_gen = 0;
    if (!node->route_lock) {
        node->route_lock = ztk_mutex_create(ZTK_MUTEX_NORMAL);
    }
}

void zrpc_route_cleanup(zrpc_node_t *node) {
    zrpc_route_t *r;
    zrpc_route_t *tmp;
    if (!node) {
        return;
    }
    if (node->route_lock) {
        ztk_mutex_lock(node->route_lock);
    }
    HASH_ITER(hh, node->routes, r, tmp) {
        HASH_DEL(node->routes, r);
        free(r);
    }
    node->routes = NULL;
    if (node->route_lock) {
        ztk_mutex_unlock(node->route_lock);
        ztk_mutex_destroy(node->route_lock);
        node->route_lock = NULL;
    }
}

static zrpc_route_t *route_upsert(zrpc_node_t *node, const char *service) {
    zrpc_route_t *r;
    HASH_FIND_STR(node->routes, service, r);
    if (!r) {
        r = (zrpc_route_t *)calloc(1, sizeof(*r));
        if (!r) {
            return NULL;
        }
        zrpc_copy_str(r->service, sizeof(r->service), service, "");
        HASH_ADD_STR(node->routes, service, r);
    }
    return r;
}

/* 由对端表重建路由（发现发生任何变更后调用）。 */
void zrpc_route_rebuild(zrpc_node_t *node) {
    int i, j, k;
    uint64_t gen;
    zrpc_route_t *r;
    zrpc_route_t *tmp;

    if (!node) {
        return;
    }
    if (node->route_lock) {
        ztk_mutex_lock(node->route_lock);
    }
    gen = ++node->route_gen;
    HASH_ITER(hh, node->routes, r, tmp) {
        if (!r->is_static) {
            r->endpoint_count = 0;
            r->next_endpoint = 0;
        }
    }
    for (i = 0; i < ZRPC_MAX_PEERS; i++) {
        zrpc_peer_t *p = &node->peers[i];
        if (!p->used || p->endpoint_count == 0) {
            continue;
        }
        for (j = 0; j < p->service_count; j++) {
            r = route_upsert(node, p->services[j]);
            if (!r || r->is_static) {
                continue;
            }
            for (k = 0; k < (int)p->endpoint_count; k++) {
                size_t e;
                int duplicate = 0;
                for (e = 0; e < r->endpoint_count; e++) {
                    if (strcmp(r->endpoints[e].scheme, p->endpoints[k].scheme) == 0 &&
                        strcmp(r->endpoints[e].host, p->endpoints[k].host) == 0 &&
                        r->endpoints[e].port == p->endpoints[k].port) {
                        duplicate = 1;
                        break;
                    }
                }
                if (!duplicate && r->endpoint_count < ZRPC_MAX_ENDPOINTS) {
                    r->endpoints[r->endpoint_count++] = p->endpoints[k];
                }
            }
            r->gen = gen;
        }
    }
    HASH_ITER(hh, node->routes, r, tmp) {
        if (r->gen != gen && !r->is_static) {
            HASH_DEL(node->routes, r);
            free(r);
        }
    }
    if (node->route_lock) {
        ztk_mutex_unlock(node->route_lock);
    }
}

int zrpc_route_set(zrpc_node_t *node, const char *service, const char *ip, uint16_t port,
                   zrpc_transport_kind_t transport) {
    zrpc_route_t *r;
    if (!node || !service || !ip || !port) {
        return ZRPC_ERR_INVALID;
    }
    if (node->route_lock) {
        ztk_mutex_lock(node->route_lock);
    }
    r = route_upsert(node, service);
    if (!r) {
        if (node->route_lock) {
            ztk_mutex_unlock(node->route_lock);
        }
        return ZRPC_ERR_NOMEM;
    }
    r->endpoint_count = 1;
    r->next_endpoint = 0;
    zrpc_copy_str(r->endpoints[0].scheme, sizeof(r->endpoints[0].scheme),
                  transport == ZRPC_TRANSPORT_TCP ? "tcp" : "udp", "udp");
    zrpc_copy_str(r->endpoints[0].host, sizeof(r->endpoints[0].host), ip, "0.0.0.0");
    r->endpoints[0].port = port;
    r->endpoints[0].transport = transport;
    r->is_static = 1;
    r->gen = node->route_gen;
    if (node->route_lock) {
        ztk_mutex_unlock(node->route_lock);
    }
    return ZRPC_OK;
}

int zrpc_route_add_endpoint(zrpc_node_t *node, const char *service, const char *uri, int is_static) {
    zrpc_endpoint_t endpoint;
    zrpc_route_t *r;
    size_t i;
    if (!node || !service || !service[0] || zrpc_endpoint_parse(uri, &endpoint) != ZRPC_OK ||
        endpoint.port == 0) {
        return ZRPC_ERR_INVALID;
    }
    if (node->route_lock) {
        ztk_mutex_lock(node->route_lock);
    }
    r = route_upsert(node, service);
    if (!r) {
        if (node->route_lock) {
            ztk_mutex_unlock(node->route_lock);
        }
        return ZRPC_ERR_NOMEM;
    }
    if (is_static && !r->is_static) {
        r->endpoint_count = 0;
        r->next_endpoint = 0;
    }
    for (i = 0; i < r->endpoint_count; i++) {
        zrpc_endpoint_t *existing = &r->endpoints[i];
        if (existing->port == endpoint.port && strcmp(existing->scheme, endpoint.scheme) == 0 &&
            strcmp(existing->host, endpoint.host) == 0) {
            r->is_static = is_static || r->is_static;
            if (node->route_lock) {
                ztk_mutex_unlock(node->route_lock);
            }
            return ZRPC_OK;
        }
    }
    if (r->endpoint_count == ZRPC_MAX_ENDPOINTS) {
        if (node->route_lock) {
            ztk_mutex_unlock(node->route_lock);
        }
        return ZRPC_ERR_NOMEM;
    }
    r->endpoints[r->endpoint_count++] = endpoint;
    r->is_static = is_static || r->is_static;
    if (node->route_lock) {
        ztk_mutex_unlock(node->route_lock);
    }
    return ZRPC_OK;
}

int zrpc_route_lookup(zrpc_node_t *node, const char *service, char *ip_out, size_t ip_cap,
                      uint16_t *port_out, zrpc_transport_kind_t *transport_out,
                      char *scheme_out, size_t scheme_cap) {
    return zrpc_route_lookup_prefer(node, service, 0, ip_out, ip_cap, port_out, transport_out,
                                    scheme_out, scheme_cap);
}

int zrpc_route_lookup_prefer(zrpc_node_t *node, const char *service, int prefer_stream,
                             char *ip_out, size_t ip_cap, uint16_t *port_out,
                             zrpc_transport_kind_t *transport_out, char *scheme_out,
                             size_t scheme_cap) {
    zrpc_route_t *r;
    int found = 0;
    if (!node || !service || !ip_out || !port_out) {
        return ZRPC_ERR_INVALID;
    }
    if (node->route_lock) {
        ztk_mutex_lock(node->route_lock);
    }
    HASH_FIND_STR(node->routes, service, r);
    if (r && r->endpoint_count) {
        zrpc_endpoint_t *endpoint = NULL;
        if (prefer_stream) {
            size_t i;
            for (i = 0; i < r->endpoint_count; i++) {
                size_t index = (r->next_endpoint + i) % r->endpoint_count;
                if (r->endpoints[index].transport == ZRPC_TRANSPORT_TCP) {
                    endpoint = &r->endpoints[index];
                    r->next_endpoint = index + 1;
                    break;
                }
            }
        }
        if (!endpoint) {
            endpoint = &r->endpoints[r->next_endpoint++ % r->endpoint_count];
        }
        zrpc_copy_str(ip_out, ip_cap, endpoint->host, "0.0.0.0");
        *port_out = endpoint->port;
        if (transport_out) {
            *transport_out = endpoint->transport;
        }
        if (scheme_out) {
            zrpc_copy_str(scheme_out, scheme_cap, endpoint->scheme, "udp");
        }
        found = 1;
    }
    if (node->route_lock) {
        ztk_mutex_unlock(node->route_lock);
    }
    return found ? ZRPC_OK : ZRPC_ERR_NOTFOUND;
}
