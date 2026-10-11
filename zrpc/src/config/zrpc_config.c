/* 简单 JSON 配置解析（内置 cJSON）。 */
#include <zrpc/zrpc_config.h>

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 取值辅助：优先读 primary（扁平顶层字段），读不到再回退 fallback（旧的 node 对象）。
 * primary/fallback 为 NULL 时 cJSON 查找返回 NULL，天然安全。
 */
static const cJSON *cfg_pick(const cJSON *primary, const cJSON *fallback, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(primary, key);
    if (item && !cJSON_IsNull(item)) {
        return item;
    }
    return fallback ? cJSON_GetObjectItemCaseSensitive(fallback, key) : NULL;
}

static void cfg_str2(char *dst, size_t cap, const cJSON *primary, const cJSON *fallback,
                     const char *key, const char *def) {
    const cJSON *item = cfg_pick(primary, fallback, key);
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
        snprintf(dst, cap, "%s", item->valuestring);
    } else if (def) {
        snprintf(dst, cap, "%s", def);
    } else {
        dst[0] = '\0';
    }
}

static void cfg_str(char *dst, size_t cap, const cJSON *obj, const char *key, const char *def) {
    cfg_str2(dst, cap, obj, NULL, key, def);
}

static unsigned cfg_u32_2(const cJSON *primary, const cJSON *fallback, const char *key,
                          unsigned def) {
    const cJSON *item = cfg_pick(primary, fallback, key);
    if (cJSON_IsNumber(item) && item->valuedouble >= 0) {
        return (unsigned)item->valuedouble;
    }
    return def;
}

static unsigned cfg_u32(const cJSON *obj, const char *key, unsigned def) {
    return cfg_u32_2(obj, NULL, key, def);
}

static zrpc_transport_kind_t cfg_transport2(const cJSON *primary, const cJSON *fallback,
                                            const char *key, zrpc_transport_kind_t def) {
    const cJSON *item = cfg_pick(primary, fallback, key);
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

static zrpc_transport_kind_t cfg_transport(const cJSON *obj, const char *key,
                                           zrpc_transport_kind_t def) {
    return cfg_transport2(obj, NULL, key, def);
}

static zrpc_node_mode_t cfg_mode2(const cJSON *primary, const cJSON *fallback, const char *key,
                                  zrpc_node_mode_t def) {
    const cJSON *item = cfg_pick(primary, fallback, key);
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

/* 解析 "host:port"（含 [ipv6]:port）；成功返回 ZRPC_OK。 */
static int cfg_host_port(const char *text, char *host, size_t host_cap, uint16_t *port) {
    zrpc_endpoint_t endpoint;
    char uri[ZRPC_ENDPOINT_MAX * 2];
    int n;
    if (!text || !host || !port) {
        return ZRPC_ERR_INVALID;
    }
    n = snprintf(uri, sizeof(uri), "udp://%s", text);
    if (n < 0 || (size_t)n >= sizeof(uri) || zrpc_endpoint_parse(uri, &endpoint) != ZRPC_OK) {
        return ZRPC_ERR_INVALID;
    }
    snprintf(host, host_cap, "%s", endpoint.host);
    *port = endpoint.port;
    return ZRPC_OK;
}

/* 配置一个 binding：接受 "scheme://host:port" 字符串或 {name,endpoint} 对象。 */
static void cfg_binding(zrpc_config_t *out, const cJSON *item) {
    zrpc_binding_endpoint_t *binding;
    if (!out || !item || out->binding_count >= ZRPC_CONFIG_MAX_BINDINGS) {
        return;
    }
    binding = &out->bindings[out->binding_count];
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(binding->endpoint, sizeof(binding->endpoint), "%s", item->valuestring);
    } else if (cJSON_IsObject(item)) {
        cfg_str(binding->name, sizeof(binding->name), item, "name", NULL);
        cfg_str(binding->endpoint, sizeof(binding->endpoint), item, "endpoint", NULL);
        if (!binding->endpoint[0]) {
            cfg_str(binding->endpoint, sizeof(binding->endpoint), item, "uri", NULL);
        }
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
    const cJSON *bindings;
    const cJSON *transport;
    const cJSON *disc;
    const cJSON *routes;
    const cJSON *peers;
    const cJSON *item;

    if (!json || !out) {
        return ZRPC_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    root = cJSON_Parse(json);
    if (!root) {
        return ZRPC_ERR_INVALID;
    }

    /* 扁平顶层字段优先，兼容旧的 "node" 对象。 */
    node = cJSON_GetObjectItemCaseSensitive(root, "node");
    cfg_str2(out->name, sizeof(out->name), root, node, "name", "zrpc");
    cfg_str2(out->bind_host, sizeof(out->bind_host), root, node, "bind_host", "0.0.0.0");
    out->node.transport = cfg_transport2(root, node, "transport", ZRPC_TRANSPORT_UDP);
    out->node.mode = cfg_mode2(root, node, "mode", ZRPC_MODE_MESH);
    out->node.data_port = (uint16_t)cfg_u32_2(root, node, "data_port", 0);
    out->node.io_threads = cfg_u32_2(root, node, "io_threads", 0);
    out->node.frag_bytes = (uint16_t)cfg_u32_2(root, node, "frag_bytes", 0);
    out->node.max_msg_bytes = cfg_u32_2(root, node, "max_msg_bytes", 0);
    out->node.backpressure_bytes = cfg_u32_2(root, node, "backpressure_bytes", 0);
    out->node.rtcp_interval_ms = cfg_u32_2(root, node, "rtcp_interval_ms", 0);
    {
        const cJSON *rtcp = cfg_pick(root, node, "rtcp");
        if (cJSON_IsBool(rtcp)) {
            out->node.rtcp_disabled = cJSON_IsTrue(rtcp) ? 0 : 1;
        }
    }
    out->node.name = out->name;
    out->node.bind_host = out->bind_host;

    /* 新格式使用顶层 bindings；兼容旧格式 transport.bindings / transport.binding。 */
    bindings = cJSON_GetObjectItemCaseSensitive(root, "bindings");
    if (cJSON_IsArray(bindings)) {
        cJSON_ArrayForEach(item, bindings) {
            cfg_binding(out, item);
        }
    } else {
        transport = cJSON_GetObjectItemCaseSensitive(root, "transport");
        if (cJSON_IsObject(transport)) {
            const cJSON *legacy_bindings =
                cJSON_GetObjectItemCaseSensitive(transport, "bindings");
            const cJSON *legacy_binding = cJSON_GetObjectItemCaseSensitive(transport, "binding");
            if (cJSON_IsArray(legacy_bindings)) {
                cJSON_ArrayForEach(item, legacy_bindings) {
                    cfg_binding(out, item);
                }
            } else if (cJSON_IsObject(legacy_binding)) {
                cfg_binding(out, legacy_binding);
            }
        }
    }

    /* discovery：新格式推荐 broadcast: "host:port"，兼容 host/port。 */
    disc = cJSON_GetObjectItemCaseSensitive(root, "discovery");
    {
        const cJSON *broadcast = cJSON_GetObjectItemCaseSensitive(disc, "broadcast");
        uint16_t broadcast_port = 0;
        char broadcast_host[ZRPC_ENDPOINT_MAX];
        if (cJSON_IsString(broadcast) && broadcast->valuestring &&
            cfg_host_port(broadcast->valuestring, broadcast_host, sizeof(broadcast_host),
                          &broadcast_port) == ZRPC_OK) {
            snprintf(out->discovery_host, sizeof(out->discovery_host), "%s", broadcast_host);
            out->node.discovery_port = broadcast_port;
        } else {
            cfg_str(out->discovery_host, sizeof(out->discovery_host), disc, "host",
                    "255.255.255.255");
            out->node.discovery_port = (uint16_t)cfg_u32(disc, "port", 0);
        }
    }
    out->node.discovery_host = out->discovery_host;
    out->node.hello_interval_ms = cfg_u32(disc, "hello_interval_ms", 0);
    out->node.node_ttl_ms = cfg_u32(disc, "node_ttl_ms", 0);
    if (out->binding_count > 0) {
        out->node.bindings = out->binding_configs;
        out->node.binding_count = out->binding_count;
    }

    /* discovery.peers：支持 "ip:port" 字符串和 {ip,port} 对象。 */
    peers = cJSON_GetObjectItemCaseSensitive(disc, "peers");
    if (cJSON_IsArray(peers)) {
        cJSON_ArrayForEach(item, peers) {
            zrpc_peer_endpoint_t *peer;
            if (out->peer_count >= ZRPC_CONFIG_MAX_PEERS) {
                break;
            }
            peer = &out->peers[out->peer_count];
            if (cJSON_IsString(item) && item->valuestring) {
                if (cfg_host_port(item->valuestring, peer->ip, sizeof(peer->ip), &peer->port) !=
                    ZRPC_OK) {
                    continue;
                }
            } else {
                cfg_str(peer->ip, sizeof(peer->ip), item, "ip", NULL);
                peer->port = (uint16_t)cfg_u32(item, "port", 0);
            }
            if (peer->ip[0] && peer->port) {
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
