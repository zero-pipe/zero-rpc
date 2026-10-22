/* 简单 JSON 配置解析（内置 cJSON）。 */
#include <config.h>

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 读取可选字符串；字段存在但类型错误或超出容量时返回 ERR_INVALID。 */
static int cfg_str(char *dst, size_t cap, const cJSON *obj, const char *key, const char *def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    int n;
    if (!item) {
        snprintf(dst, cap, "%s", def ? def : "");
        return OK;
    }
    if (!cJSON_IsString(item) || !item->valuestring) {
        return ERR_INVALID;
    }
    n = snprintf(dst, cap, "%s", item->valuestring);
    if (n < 0 || (size_t)n >= cap) {
        return ERR_INVALID;
    }
    return OK;
}

/* 读取可选 u32，并校验整数与范围；缺省返回 def。 */
static int cfg_u32(const cJSON *obj,
                   const char *key,
                   uint32_t def,
                   uint32_t min,
                   uint32_t max,
                   uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    double d;
    uint32_t v;
    if (!item) {
        *out = def;
        return OK;
    }
    if (!cJSON_IsNumber(item)) {
        return ERR_INVALID;
    }
    d = item->valuedouble;
    if (d != d || d < 0.0 || d > (double)UINT32_MAX) {
        return ERR_INVALID;
    }
    v = (uint32_t)d;
    if ((double)v != d || v < min || v > max) {
        return ERR_INVALID;
    }
    *out = v;
    return OK;
}

static int cfg_transport(const cJSON *obj,
                         const char *key,
                         transport_kind_t def,
                         transport_kind_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item) {
        *out = def;
        return OK;
    }
    if (!cJSON_IsString(item) || !item->valuestring) {
        return ERR_INVALID;
    }
    if (strcmp(item->valuestring, "tcp") == 0 || strcmp(item->valuestring, "TCP") == 0) {
        *out = TRANSPORT_TCP;
        return OK;
    }
    if (strcmp(item->valuestring, "udp") == 0 || strcmp(item->valuestring, "UDP") == 0) {
        *out = TRANSPORT_UDP;
        return OK;
    }
    return ERR_INVALID;
}

static int cfg_mode(const cJSON *obj, const char *key, node_mode_t def, node_mode_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item) {
        *out = def;
        return OK;
    }
    if (!cJSON_IsString(item) || !item->valuestring) {
        return ERR_INVALID;
    }
    if (strcmp(item->valuestring, "static") == 0 || strcmp(item->valuestring, "STATIC") == 0) {
        *out = MODE_STATIC;
        return OK;
    }
    if (strcmp(item->valuestring, "mesh") == 0 || strcmp(item->valuestring, "MESH") == 0) {
        *out = MODE_MESH;
        return OK;
    }
    return ERR_INVALID;
}

/* 解析 "host:port"（含 [ipv6]:port）；成功返回 OK。 */
static int cfg_host_port(const char *text, char *host, size_t host_cap, uint16_t *port)
{
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
static int cfg_binding(config_t *out, const cJSON *item)
{
    binding_endpoint_t *binding;
    endpoint_t endpoint;
    int n;
    if (!out || !item || out->binding_count >= CONFIG_MAX_BINDINGS) {
        return ERR_INVALID;
    }
    binding = &out->bindings[out->binding_count];
    if (cJSON_IsString(item) && item->valuestring) {
        n = snprintf(binding->endpoint, sizeof(binding->endpoint), "%s", item->valuestring);
        if (n < 0 || (size_t)n >= sizeof(binding->endpoint)) {
            return ERR_INVALID;
        }
    } else if (cJSON_IsObject(item)) {
        if (cfg_str(binding->name, sizeof(binding->name), item, "name", NULL) != OK) {
            return ERR_INVALID;
        }
        if (cfg_str(binding->endpoint, sizeof(binding->endpoint), item, "endpoint", NULL) != OK) {
            return ERR_INVALID;
        }
        if (!binding->endpoint[0] &&
            cfg_str(binding->endpoint, sizeof(binding->endpoint), item, "uri", NULL) != OK) {
            return ERR_INVALID;
        }
    } else {
        return ERR_INVALID;
    }
    if (!binding->endpoint[0] || endpoint_parse(binding->endpoint, &endpoint) != OK) {
        return ERR_INVALID;
    }
    if (!binding->name[0]) {
        snprintf(binding->name, sizeof(binding->name), "%s", endpoint.scheme);
    }
    if (!binding->name[0]) {
        return ERR_INVALID;
    }
    out->binding_configs[out->binding_count].name = binding->name;
    out->binding_configs[out->binding_count].endpoint = binding->endpoint;
    out->binding_count++;
    return OK;
}

/* ---- 配置加载 ---- */

int config_load(const char *json, config_t *out)
{
    cJSON *root;
    const cJSON *bindings;
    const cJSON *disc;
    const cJSON *routes;
    const cJSON *peers;
    const cJSON *item;
    uint32_t v;

    if (!json || !out) {
        return ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    root = cJSON_Parse(json);
    if (!root) {
        return ERR_INVALID;
    }

    if (cfg_str(out->name, sizeof(out->name), root, "name", "rpc") != OK ||
        cfg_str(out->bind_host, sizeof(out->bind_host), root, "bind_host", "0.0.0.0") != OK ||
        cfg_transport(root, "transport", TRANSPORT_UDP, &out->node.transport) != OK ||
        cfg_mode(root, "mode", MODE_MESH, &out->node.mode) != OK) {
        goto invalid;
    }
    if (cfg_u32(root, "data_port", 0, 0, UINT16_MAX, &v) != OK) {
        goto invalid;
    }
    out->node.data_port = (uint16_t)v;
    if (cfg_u32(root, "io_threads", 0, 0, 1024, &out->node.io_threads) != OK ||
        cfg_u32(root, "handler_threads", 0, 0, 1024, &out->node.handler_threads) != OK ||
        cfg_u32(root,
                "handler_max_concurrency",
                0,
                0,
                UINT32_MAX,
                &out->node.handler_max_concurrency) != OK ||
        cfg_u32(root, "max_inflight", 0, 0, UINT32_MAX, &out->node.max_inflight) != OK) {
        goto invalid;
    }
    if (cfg_u32(root, "frag_bytes", 0, 0, UINT16_MAX, &v) != OK) {
        goto invalid;
    }
    out->node.frag_bytes = (uint16_t)v;
    if (cfg_u32(root, "max_msg_bytes", 0, 0, UINT32_MAX, &out->node.max_msg_bytes) != OK ||
        cfg_u32(root,
                "backpressure_bytes",
                0,
                0,
                UINT32_MAX,
                &out->node.backpressure_bytes) != OK ||
        cfg_u32(root, "rtcp_interval_ms", 0, 0, UINT32_MAX, &out->node.rtcp_interval_ms) != OK ||
        cfg_u32(root, "max_streams", 0, 0, UINT32_MAX, &out->node.max_streams) != OK ||
        cfg_u32(root,
                "peer_rate_limit_pps",
                0,
                0,
                UINT32_MAX,
                &out->node.peer_rate_limit_pps) != OK) {
        goto invalid;
    }
    {
        const cJSON *rtcp = cJSON_GetObjectItemCaseSensitive(root, "rtcp");
        if (rtcp && !cJSON_IsBool(rtcp)) {
            goto invalid;
        }
        if (rtcp) {
            out->node.rtcp_disabled = cJSON_IsTrue(rtcp) ? 0 : 1;
        }
    }
    out->node.name = out->name;
    out->node.bind_host = out->bind_host;

    /* bindings 使用顶层数组，元素为 URI 字符串或 {name, endpoint} 对象。 */
    bindings = cJSON_GetObjectItemCaseSensitive(root, "bindings");
    if (bindings && !cJSON_IsArray(bindings)) {
        goto invalid;
    }
    if (bindings) {
        cJSON_ArrayForEach(item, bindings)
        {
            if (cfg_binding(out, item) != OK) {
                goto invalid;
            }
        }
    }

    /* discovery.broadcast 使用 "host:port"。 */
    disc = cJSON_GetObjectItemCaseSensitive(root, "discovery");
    if (disc && !cJSON_IsObject(disc)) {
        goto invalid;
    }
    {
        const cJSON *broadcast = cJSON_GetObjectItemCaseSensitive(disc, "broadcast");
        uint16_t broadcast_port = 0;
        char broadcast_host[ENDPOINT_MAX];
        if (broadcast &&
            (!cJSON_IsString(broadcast) || !broadcast->valuestring ||
             cfg_host_port(
                 broadcast->valuestring, broadcast_host, sizeof(broadcast_host), &broadcast_port) !=
                 OK)) {
            goto invalid;
        }
        if (broadcast) {
            snprintf(out->discovery_host, sizeof(out->discovery_host), "%s", broadcast_host);
            out->node.discovery_port = broadcast_port;
        } else {
            snprintf(out->discovery_host, sizeof(out->discovery_host), "%s", "255.255.255.255");
            out->node.discovery_port = 0;
        }
    }
    out->node.discovery_host = out->discovery_host;
    if (cfg_u32(disc, "hello_interval_ms", 0, 0, UINT32_MAX, &out->node.hello_interval_ms) != OK ||
        cfg_u32(disc, "node_ttl_ms", 0, 0, UINT32_MAX, &out->node.node_ttl_ms) != OK) {
        goto invalid;
    }
    if (out->binding_count > 0) {
        out->node.bindings = out->binding_configs;
        out->node.binding_count = out->binding_count;
    }

    /* discovery.peers：支持 "ip:port" 字符串和 {ip,port} 对象。 */
    peers = cJSON_GetObjectItemCaseSensitive(disc, "peers");
    if (peers && !cJSON_IsArray(peers)) {
        goto invalid;
    }
    if (peers) {
        cJSON_ArrayForEach(item, peers)
        {
            peer_endpoint_t *peer;
            if (out->peer_count >= CONFIG_MAX_PEERS) {
                goto invalid;
            }
            peer = &out->peers[out->peer_count];
            if (!cJSON_IsString(item) || !item->valuestring ||
                cfg_host_port(item->valuestring, peer->ip, sizeof(peer->ip), &peer->port) != OK) {
                goto invalid;
            }
            if (peer->ip[0] && peer->port) {
                out->peer_count++;
            } else {
                goto invalid;
            }
        }
    }

    /* routes */
    routes = cJSON_GetObjectItemCaseSensitive(root, "routes");
    if (routes && !cJSON_IsArray(routes)) {
        goto invalid;
    }
    if (routes) {
        cJSON_ArrayForEach(item, routes)
        {
            static_route_t *r;
            if (out->route_count >= CONFIG_MAX_ROUTES) {
                goto invalid;
            }
            if (!cJSON_IsObject(item)) {
                goto invalid;
            }
            r = &out->routes[out->route_count];
            if (cfg_str(r->service, sizeof(r->service), item, "service", NULL) != OK ||
                cfg_str(r->endpoint, sizeof(r->endpoint), item, "endpoint", NULL) != OK) {
                goto invalid;
            }
            if (!r->service[0] || !r->endpoint[0]) {
                goto invalid;
            }
            {
                endpoint_t endpoint;
                if (endpoint_parse(r->endpoint, &endpoint) != OK) {
                    goto invalid;
                }
            }
            out->route_count++;
        }
    }
    cJSON_Delete(root);
    return OK;

invalid:
    cJSON_Delete(root);
    memset(out, 0, sizeof(*out));
    return ERR_INVALID;
}

int config_load_file(const char *path, config_t *out)
{
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

int node_create_from_config(const config_t *cfg, node_t **out)
{
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
        (void)node_add_static_endpoint(*out, cfg->routes[i].service, cfg->routes[i].endpoint);
    }
    return OK;
}
