/* 简单 JSON 配置解析（内置 cJSON）。 */
#include <zrpc/zrpc_config.h>

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void cfg_str(char *dst, size_t cap, const cJSON *obj, const char *key, const char *def) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
        snprintf(dst, cap, "%s", item->valuestring);
    } else if (def) {
        snprintf(dst, cap, "%s", def);
    } else {
        dst[0] = '\0';
    }
}

static unsigned cfg_u32(const cJSON *obj, const char *key, unsigned def) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item) && item->valuedouble >= 0) {
        return (unsigned)item->valuedouble;
    }
    return def;
}

static zrpc_transport_kind_t cfg_transport(const cJSON *obj, const char *key,
                                           zrpc_transport_kind_t def) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        if (strcmp(item->valuestring, "tcp") == 0 || strcmp(item->valuestring, "TCP") == 0) {
            return ZRPC_TRANSPORT_TCP;
        }
        if (strcmp(item->valuestring, "udp") == 0 || strcmp(item->valuestring, "UDP") == 0) {
            return ZRPC_TRANSPORT_UDP;
        }
    }
    return def;
}

static zrpc_node_mode_t cfg_mode(const cJSON *obj, const char *key, zrpc_node_mode_t def) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        if (strcmp(item->valuestring, "static") == 0 || strcmp(item->valuestring, "STATIC") == 0) {
            return ZRPC_MODE_STATIC;
        }
        if (strcmp(item->valuestring, "mesh") == 0 || strcmp(item->valuestring, "MESH") == 0) {
            return ZRPC_MODE_MESH;
        }
    }
    return def;
}

static void cfg_binding(zrpc_config_t *out, const cJSON *item) {
    zrpc_binding_endpoint_t *binding;
    if (!out || !item || out->binding_count >= ZRPC_CONFIG_MAX_BINDINGS) {
        return;
    }
    binding = &out->bindings[out->binding_count];
    cfg_str(binding->name, sizeof(binding->name), item, "name", NULL);
    cfg_str(binding->endpoint, sizeof(binding->endpoint), item, "endpoint", NULL);
    if (!binding->endpoint[0] && binding->name[0]) {
        cfg_str(binding->endpoint, sizeof(binding->endpoint), item, "uri", NULL);
    }
    if (!binding->endpoint[0]) {
        return;
    }
    if (!binding->name[0]) {
        zrpc_endpoint_t endpoint;
        if (zrpc_endpoint_parse(binding->endpoint, &endpoint) == ZRPC_OK) {
            snprintf(binding->name, sizeof(binding->name), "%s", endpoint.scheme);
        }
    }
    if (!binding->name[0]) {
        return;
    }
    out->binding_configs[out->binding_count].name = binding->name;
    out->binding_configs[out->binding_count].endpoint = binding->endpoint;
    out->binding_count++;
}

int zrpc_config_load(const char *json, zrpc_config_t *out) {
    cJSON *root;
    const cJSON *node;
    const cJSON *disc;
    const cJSON *routes;
    const cJSON *peers;
    const cJSON *item;
    const cJSON *transport;
    int i;

    if (!json || !out) {
        return ZRPC_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    root = cJSON_Parse(json);
    if (!root) {
        return ZRPC_ERR_INVALID;
    }

    /* node */
    node = cJSON_GetObjectItemCaseSensitive(root, "node");
    cfg_str(out->name, sizeof(out->name), node, "name", "zrpc");
    cfg_str(out->bind_host, sizeof(out->bind_host), node, "bind_host", "0.0.0.0");
    out->node.transport = cfg_transport(node, "transport", ZRPC_TRANSPORT_UDP);
    out->node.mode = cfg_mode(node, "mode", ZRPC_MODE_MESH);
    out->node.data_port = (uint16_t)cfg_u32(node, "data_port", 0);
    out->node.io_threads = cfg_u32(node, "io_threads", 0);
    out->node.frag_bytes = (uint16_t)cfg_u32(node, "frag_bytes", 0);
    out->node.max_msg_bytes = cfg_u32(node, "max_msg_bytes", 0);
    out->node.backpressure_bytes = cfg_u32(node, "backpressure_bytes", 0);
    out->node.name = out->name;
    out->node.bind_host = out->bind_host;

    /* New binding model: transport.bindings or transport.binding. */
    transport = cJSON_GetObjectItemCaseSensitive(root, "transport");
    if (cJSON_IsObject(transport)) {
        const cJSON *bindings = cJSON_GetObjectItemCaseSensitive(transport, "bindings");
        const cJSON *binding = cJSON_GetObjectItemCaseSensitive(transport, "binding");
        if (cJSON_IsArray(bindings)) {
            cJSON_ArrayForEach(item, bindings) {
                cfg_binding(out, item);
            }
        } else if (cJSON_IsObject(binding)) {
            cfg_binding(out, binding);
        }
    }

    /* discovery */
    disc = cJSON_GetObjectItemCaseSensitive(root, "discovery");
    cfg_str(out->discovery_host, sizeof(out->discovery_host), disc, "host", "255.255.255.255");
    out->node.discovery_host = out->discovery_host;
    out->node.discovery_port = (uint16_t)cfg_u32(disc, "port", 0);
    out->node.hello_interval_ms = cfg_u32(disc, "hello_interval_ms", 0);
    out->node.node_ttl_ms = cfg_u32(disc, "node_ttl_ms", 0);
    if (out->binding_count > 0) {
        out->node.bindings = out->binding_configs;
        out->node.binding_count = out->binding_count;
    }

    /* discovery.peers */
    peers = cJSON_GetObjectItemCaseSensitive(disc, "peers");
    if (cJSON_IsArray(peers)) {
        cJSON_ArrayForEach(item, peers) {
            if (out->peer_count >= ZRPC_CONFIG_MAX_PEERS) {
                break;
            }
            cfg_str(out->peers[out->peer_count].ip, sizeof(out->peers[0].ip), item, "ip", NULL);
            out->peers[out->peer_count].port = (uint16_t)cfg_u32(item, "port", 0);
            if (out->peers[out->peer_count].ip[0] && out->peers[out->peer_count].port) {
                out->peer_count++;
            }
        }
    }

    /* routes */
    routes = cJSON_GetObjectItemCaseSensitive(root, "routes");
    if (cJSON_IsArray(routes)) {
        cJSON_ArrayForEach(item, routes) {
            zrpc_static_route_t *r;
            if (out->route_count >= ZRPC_CONFIG_MAX_ROUTES) {
                break;
            }
            r = &out->routes[out->route_count];
            cfg_str(r->service, sizeof(r->service), item, "service", NULL);
            cfg_str(r->endpoint, sizeof(r->endpoint), item, "endpoint", NULL);
            cfg_str(r->ip, sizeof(r->ip), item, "ip", NULL);
            r->port = (uint16_t)cfg_u32(item, "port", 0);
            r->transport = cfg_transport(item, "transport", out->node.transport);
            if (r->service[0] && ((r->endpoint[0]) || (r->ip[0] && r->port))) {
                out->route_count++;
            }
        }
    }
    (void)i;
    cJSON_Delete(root);
    return ZRPC_OK;
}

int zrpc_config_load_file(const char *path, zrpc_config_t *out) {
    FILE *f;
    long size;
    char *buf;
    int rc;

    if (!path || !out) {
        return ZRPC_ERR_INVALID;
    }
    f = fopen(path, "rb");
    if (!f) {
        return ZRPC_ERR_IO;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ZRPC_ERR_IO;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return ZRPC_ERR_IO;
    }
    rewind(f);
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return ZRPC_ERR_NOMEM;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return ZRPC_ERR_IO;
    }
    buf[size] = '\0';
    fclose(f);
    rc = zrpc_config_load(buf, out);
    free(buf);
    return rc;
}

int zrpc_node_create_from_config(const zrpc_config_t *cfg, zrpc_node_t **out) {
    int rc;
    size_t i;
    if (!cfg || !out) {
        return ZRPC_ERR_INVALID;
    }
    rc = zrpc_node_create(&cfg->node, out);
    if (rc != ZRPC_OK) {
        return rc;
    }
    for (i = 0; i < cfg->peer_count; i++) {
        (void)zrpc_node_add_discovery_peer(*out, cfg->peers[i].ip, cfg->peers[i].port);
    }
    for (i = 0; i < cfg->route_count; i++) {
        if (cfg->routes[i].endpoint[0]) {
            (void)zrpc_node_add_static_endpoint(*out, cfg->routes[i].service,
                                                cfg->routes[i].endpoint);
        } else {
            (void)zrpc_node_add_static_route(*out, cfg->routes[i].service, cfg->routes[i].ip,
                                             cfg->routes[i].port, cfg->routes[i].transport);
        }
    }
    return ZRPC_OK;
}
