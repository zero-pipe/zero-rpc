/* 简单 JSON 配置解析（内置 cJSON）。 */
#include <config.h>

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

static transport_kind_t cfg_transport(const cJSON *obj, const char *key,
                                      transport_kind_t def) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        if (strcmp(item->valuestring, "tcp") == 0 || strcmp(item->valuestring, "TCP") == 0) {
            return TRANSPORT_TCP;
        }
        if (strcmp(item->valuestring, "udp") == 0 || strcmp(item->valuestring, "UDP") == 0) {
            return TRANSPORT_UDP;
        }
    }
    return def;
}

static node_mode_t cfg_mode(const cJSON *obj, const char *key, node_mode_t def) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        if (strcmp(item->valuestring, "static") == 0 || strcmp(item->valuestring, "STATIC") == 0) {
            return MODE_STATIC;
        }
        if (strcmp(item->valuestring, "mesh") == 0 || strcmp(item->valuestring, "MESH") == 0) {
            return MODE_MESH;
        }
    }
    return def;
}

/* 解析 "host:port"（含 [ipv6]:port）；成功返回 OK。 */
static int cfg_host_port(const char *text, char *host, size_t host_cap, uint16_t *port) {
    endpoint_t endpoint;
    char uri[ENDPOINT_MAX * 2];
    int n;
    if (!text || !host || !port) {
        return ERR_INVALID;
    }
    n = snprintf(uri, sizeof(uri), "udp://%s", text);
    if (n < 0 || (size_t)n >= sizeof(uri) || endpoint_parse(uri, &endpoint) != OK) {
        return ERR_INVALID;
    }
    snprintf(host, host_cap, "%s", endpoint.host);
    *port = endpoint.port;
    return OK;
}

/* 配置一个 binding：接受 "scheme://host:port" 字符串或 {name,endpoint} 对象。 */
static void cfg_binding(config_t *out, const cJSON *item) {
    binding_endpoint_t *binding;
    if (!out || !item || out->binding_count >= CONFIG_MAX_BINDINGS) {
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
        endpoint_t endpoint;
        if (endpoint_parse(binding->endpoint, &endpoint) == OK) {
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

/* ---- 配置加载 ---- */

int config_load(const char *json, config_t *out) {
    cJSON *root;
    const cJSON *bindings;
    const cJSON *disc;
    const cJSON *routes;
    const cJSON *peers;
    const cJSON *item;

    if (!json || !out) {
        return ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    root = cJSON_Parse(json);
    if (!root) {
        return ERR_INVALID;
    }

    cfg_str(out->name, sizeof(out->name), root, "name", "rpc");
    cfg_str(out->bind_host, sizeof(out->bind_host), root, "bind_host", "0.0.0.0");
    out->node.transport = cfg_transport(root, "transport", TRANSPORT_UDP);
    out->node.mode = cfg_mode(root, "mode", MODE_MESH);
    out->node.data_port = (uint16_t)cfg_u32(root, "data_port", 0);
    out->node.io_threads = cfg_u32(root, "io_threads", 0);
    out->node.handler_threads = cfg_u32(root, "handler_threads", 0);
    out->node.handler_max_concurrency = cfg_u32(root, "handler_max_concurrency", 0);
    out->node.frag_bytes = (uint16_t)cfg_u32(root, "frag_bytes", 0);
    out->node.max_msg_bytes = cfg_u32(root, "max_msg_bytes", 0);
    out->node.backpressure_bytes = cfg_u32(root, "backpressure_bytes", 0);
    out->node.rtcp_interval_ms = cfg_u32(root, "rtcp_interval_ms", 0);
    {
        const cJSON *rtcp = cJSON_GetObjectItemCaseSensitive(root, "rtcp");
        if (cJSON_IsBool(rtcp)) {
            out->node.rtcp_disabled = cJSON_IsTrue(rtcp) ? 0 : 1;
        }
    }
    out->node.name = out->name;
    out->node.bind_host = out->bind_host;

    /* bindings 使用顶层数组，元素为 URI 字符串或 {name, endpoint} 对象。 */
    bindings = cJSON_GetObjectItemCaseSensitive(root, "bindings");
    if (cJSON_IsArray(bindings)) {
        cJSON_ArrayForEach(item, bindings) {
            cfg_binding(out, item);
        }
    }

    /* discovery.broadcast 使用 "host:port"。 */
    disc = cJSON_GetObjectItemCaseSensitive(root, "discovery");
    {
        const cJSON *broadcast = cJSON_GetObjectItemCaseSensitive(disc, "broadcast");
        uint16_t broadcast_port = 0;
        char broadcast_host[ENDPOINT_MAX];
        if (cJSON_IsString(broadcast) && broadcast->valuestring &&
            cfg_host_port(broadcast->valuestring, broadcast_host, sizeof(broadcast_host),
                          &broadcast_port) == OK) {
            snprintf(out->discovery_host, sizeof(out->discovery_host), "%s", broadcast_host);
            out->node.discovery_port = broadcast_port;
        } else {
            snprintf(out->discovery_host, sizeof(out->discovery_host), "%s",
                     "255.255.255.255");
            out->node.discovery_port = 0;
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
            peer_endpoint_t *peer;
            if (out->peer_count >= CONFIG_MAX_PEERS) {
                break;
            }
            peer = &out->peers[out->peer_count];
            if (!cJSON_IsString(item) || !item->valuestring ||
                cfg_host_port(item->valuestring, peer->ip, sizeof(peer->ip), &peer->port) != OK) {
                continue;
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
            static_route_t *r;
            if (out->route_count >= CONFIG_MAX_ROUTES) {
                break;
            }
            r = &out->routes[out->route_count];
            cfg_str(r->service, sizeof(r->service), item, "service", NULL);
            cfg_str(r->endpoint, sizeof(r->endpoint), item, "endpoint", NULL);
            if (r->service[0] && r->endpoint[0]) {
                out->route_count++;
            }
        }
    }
    cJSON_Delete(root);
    return OK;
}

int config_load_file(const char *path, config_t *out) {
    FILE *f;
    long size;
    char *buf;
    int rc;

    if (!path || !out) {
        return ERR_INVALID;
    }
    f = fopen(path, "rb");
    if (!f) {
        return ERR_IO;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ERR_IO;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return ERR_IO;
    }
    rewind(f);
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return ERR_NOMEM;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return ERR_IO;
    }
    buf[size] = '\0';
    fclose(f);
    rc = config_load(buf, out);
    free(buf);
    return rc;
}

/* ---- 节点创建 ---- */

int node_create_from_config(const config_t *cfg, node_t **out) {
    int rc;
    size_t i;
    if (!cfg || !out) {
        return ERR_INVALID;
    }
    rc = node_create(&cfg->node, out);
    if (rc != OK) {
        return rc;
    }
    for (i = 0; i < cfg->peer_count; i++) {
        (void)node_add_discovery_peer(*out, cfg->peers[i].ip, cfg->peers[i].port);
    }
    for (i = 0; i < cfg->route_count; i++) {
        (void)node_add_static_endpoint(*out, cfg->routes[i].service,
                                            cfg->routes[i].endpoint);
    }
    return OK;
}
