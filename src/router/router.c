/* 路由表：service -> 目标端点（自带锁，操作 nb_route_table_t，不依赖 node）。 */
#include "router.h"

#include "util.h"

#include <ntk.h>

#include <stdlib.h>
#include <string.h>

void nb_route_init(nb_route_table_t *table)
{
    if (!table) {
        return;
    }
    table->routes = NULL;
    if (!table->lock) {
        table->lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    }
}

void nb_route_cleanup(nb_route_table_t *table)
{
    nb_route_t *r;
    nb_route_t *tmp;
    if (!table) {
        return;
    }
    if (table->lock) {
        ntk_mutex_lock((ntk_mutex *)table->lock);
    }
    HASH_ITER(hh, table->routes, r, tmp)
    {
        HASH_DEL(table->routes, r);
        free(r);
    }
    table->routes = NULL;
    if (table->lock) {
        ntk_mutex_unlock((ntk_mutex *)table->lock);
        ntk_mutex_destroy((ntk_mutex *)table->lock);
        table->lock = NULL;
    }
}

static nb_route_t *nb_route_upsert(nb_route_table_t *table, const char *service)
{
    nb_route_t *r;
    HASH_FIND_STR(table->routes, service, r);
    if (!r) {
        r = (nb_route_t *)calloc(1, sizeof(*r));
        if (!r) {
            return NULL;
        }
        nb_copy_str(r->service, sizeof(r->service), service, "");
        HASH_ADD_STR(table->routes, service, r);
    }
    return r;
}

int nb_route_set(nb_route_table_t *table,
                  const char *service,
                  const char *ip,
                  uint16_t port,
                  nb_transport_kind_t transport)
{
    nb_route_t *r;
    if (!table || !service || !ip || !port) {
        return NB_ERR_INVALID;
    }
    if (table->lock) {
        ntk_mutex_lock((ntk_mutex *)table->lock);
    }
    r = nb_route_upsert(table, service);
    if (!r) {
        if (table->lock) {
            ntk_mutex_unlock((ntk_mutex *)table->lock);
        }
        return NB_ERR_NOMEM;
    }
    r->endpoint_count = 1;
    r->next_endpoint = 0;
    nb_copy_str(r->endpoints[0].scheme,
                 sizeof(r->endpoints[0].scheme),
                 transport == NB_TRANSPORT_TCP ? "tcp" : "udp",
                 "udp");
    nb_copy_str(r->endpoints[0].host, sizeof(r->endpoints[0].host), ip, "0.0.0.0");
    r->endpoints[0].port = port;
    r->endpoints[0].transport = transport;
    r->is_static = 1;
    if (table->lock) {
        ntk_mutex_unlock((ntk_mutex *)table->lock);
    }
    return NB_OK;
}

int nb_route_add_endpoint(nb_route_table_t *table,
                           const char *service,
                           const char *uri,
                           int is_static)
{
    nb_endpoint_t endpoint;
    nb_route_t *r;
    size_t i;
    if (!table || !service || !service[0] || nb_endpoint_parse(uri, &endpoint) != NB_OK ||
        endpoint.port == 0) {
        return NB_ERR_INVALID;
    }
    if (table->lock) {
        ntk_mutex_lock((ntk_mutex *)table->lock);
    }
    r = nb_route_upsert(table, service);
    if (!r) {
        if (table->lock) {
            ntk_mutex_unlock((ntk_mutex *)table->lock);
        }
        return NB_ERR_NOMEM;
    }
    if (is_static && !r->is_static) {
        r->endpoint_count = 0;
        r->next_endpoint = 0;
    }
    for (i = 0; i < r->endpoint_count; i++) {
        nb_endpoint_t *existing = &r->endpoints[i];
        if (existing->port == endpoint.port && strcmp(existing->scheme, endpoint.scheme) == 0 &&
            strcmp(existing->host, endpoint.host) == 0) {
            r->is_static = is_static || r->is_static;
            if (table->lock) {
                ntk_mutex_unlock((ntk_mutex *)table->lock);
            }
            return NB_OK;
        }
    }
    if (r->endpoint_count == NB_MAX_ENDPOINTS) {
        if (table->lock) {
            ntk_mutex_unlock((ntk_mutex *)table->lock);
        }
        return NB_ERR_NOMEM;
    }
    r->endpoints[r->endpoint_count++] = endpoint;
    r->is_static = is_static || r->is_static;
    if (table->lock) {
        ntk_mutex_unlock((ntk_mutex *)table->lock);
    }
    return NB_OK;
}

int nb_route_lookup(nb_route_table_t *table,
                     const char *service,
                     char *ip_out,
                     size_t ip_cap,
                     uint16_t *port_out,
                     nb_transport_kind_t *transport_out,
                     char *scheme_out,
                     size_t scheme_cap)
{
    return nb_route_lookup_prefer(
        table, service, 0, ip_out, ip_cap, port_out, transport_out, scheme_out, scheme_cap);
}

int nb_route_lookup_prefer(nb_route_table_t *table,
                            const char *service,
                            int prefer_stream,
                            char *ip_out,
                            size_t ip_cap,
                            uint16_t *port_out,
                            nb_transport_kind_t *transport_out,
                            char *scheme_out,
                            size_t scheme_cap)
{
    nb_route_t *r;
    int found = 0;
    if (!table || !service || !ip_out || !port_out) {
        return NB_ERR_INVALID;
    }
    if (table->lock) {
        ntk_mutex_lock((ntk_mutex *)table->lock);
    }
    HASH_FIND_STR(table->routes, service, r);
    if (r && r->endpoint_count) {
        nb_endpoint_t *endpoint = NULL;
        if (prefer_stream) {
            size_t i;
            for (i = 0; i < r->endpoint_count; i++) {
                size_t index = (r->next_endpoint + i) % r->endpoint_count;
                if (r->endpoints[index].transport == NB_TRANSPORT_TCP) {
                    endpoint = &r->endpoints[index];
                    r->next_endpoint = index + 1;
                    break;
                }
            }
        }
        if (!endpoint) {
            endpoint = &r->endpoints[r->next_endpoint++ % r->endpoint_count];
        }
        nb_copy_str(ip_out, ip_cap, endpoint->host, "0.0.0.0");
        *port_out = endpoint->port;
        if (transport_out) {
            *transport_out = endpoint->transport;
        }
        if (scheme_out) {
            nb_copy_str(scheme_out, scheme_cap, endpoint->scheme, "udp");
        }
        found = 1;
    }
    if (table->lock) {
        ntk_mutex_unlock((ntk_mutex *)table->lock);
    }
    return found ? NB_OK : NB_ERR_NOTFOUND;
}
