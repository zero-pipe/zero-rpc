/*
 * 配置加载：模型驱动的 JSON 解析 + 旧的扁平 JSON 兼容层。
 *
 * 新格式按模块拆成子对象（runtime / transport / discovery / metrics / log），
 * 由 codec_json 的 model 描述字段布局后统一填充；旧格式把同样的字段平铺在
 * 顶层（transport 为字符串、bindings 为 URI 数组），这里在解析时映射进对应
 * 子结构。两种格式共用同一套校验与默认值。
 */
#include <config.h>
#include <node.h>

#include "cJSON.h"
#include "codec_json_internal.h"
#include "defaults.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 默认值 ---- */

void node_runtime_config_default(node_runtime_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
}

void node_transport_config_default(node_transport_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->transport = TRANSPORT_UDP;
    cfg->bind_host = ZRI_DEFAULT_BIND_HOST;
}

void node_discovery_config_default(node_discovery_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->host = ZRI_DEFAULT_DISCOVERY_HOST;
}

void node_metrics_config_default(node_metrics_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->sample_every = ZRI_DEFAULT_METRICS_SAMPLE_EVERY;
}

void node_log_config_default(node_log_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->level = NODE_LOG_INFO;
}

void node_config_default(node_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->name = ZRI_DEFAULT_NODE_NAME;
    cfg->mode = MODE_MESH;
    node_runtime_config_default(&cfg->runtime);
    node_transport_config_default(&cfg->transport);
    node_discovery_config_default(&cfg->discovery);
    node_metrics_config_default(&cfg->metrics);
    node_log_config_default(&cfg->log);
}

/* ---- 模型驱动的解析中间结构 ---- */

typedef struct cfg_runtime_in {
    uint32_t io_threads;
    uint32_t handler_threads;
    uint32_t handler_max_concurrency;
    uint32_t max_inflight;
    uint32_t max_streams;
    uint32_t peer_rate_limit_pps;
} cfg_runtime_in_t;

typedef struct cfg_transport_in {
    char *kind;
    char *bind_host;
    uint32_t data_port;
    uint32_t frag_bytes;
    uint32_t max_msg_bytes;
    uint32_t backpressure_bytes;
    uint32_t rtcp_interval_ms;
    uint8_t rtcp;
    uint8_t rtcp_present;
} cfg_transport_in_t;

typedef struct cfg_discovery_in {
    char *mode;
    char *broadcast;
    uint32_t hello_interval_ms;
    uint32_t node_ttl_ms;
} cfg_discovery_in_t;

typedef struct cfg_metrics_in {
    uint8_t disabled;
    uint8_t latency_disabled;
    uint32_t sample_every;
    uint8_t per_peer_disabled;
} cfg_metrics_in_t;

typedef struct cfg_log_in {
    uint8_t disabled;
    char *level;
    char *sink;
    char *file;
} cfg_log_in_t;

typedef struct cfg_route_in {
    char *service;
    char *endpoint;
} cfg_route_in_t;

typedef struct cfg_binding_in {
    char *name;
    char *endpoint;
} cfg_binding_in_t;

/* 用显式结构体字段偏移描述每个子对象，交给 codec_json 填充。 */
static const nb_codec_json_model_t g_runtime_model[] = {
    { NB_CODEC_JSON_OBJ, NULL, 0, 0, { .obj_size = sizeof(cfg_runtime_in_t) } },
    { NB_CODEC_JSON_U32, "io_threads", offsetof(cfg_runtime_in_t, io_threads), 0, { 0 } },
    { NB_CODEC_JSON_U32, "handler_threads", offsetof(cfg_runtime_in_t, handler_threads), 0, { 0 } },
    { NB_CODEC_JSON_U32,
      "handler_max_concurrency",
      offsetof(cfg_runtime_in_t, handler_max_concurrency),
      0,
      { 0 } },
    { NB_CODEC_JSON_U32, "max_inflight", offsetof(cfg_runtime_in_t, max_inflight), 0, { 0 } },
    { NB_CODEC_JSON_U32, "max_streams", offsetof(cfg_runtime_in_t, max_streams), 0, { 0 } },
    { NB_CODEC_JSON_U32,
      "peer_rate_limit_pps",
      offsetof(cfg_runtime_in_t, peer_rate_limit_pps),
      0,
      { 0 } },
};

static const nb_codec_json_model_t g_transport_model[] = {
    { NB_CODEC_JSON_OBJ, NULL, 0, 0, { .obj_size = sizeof(cfg_transport_in_t) } },
    { NB_CODEC_JSON_STRING, "kind", offsetof(cfg_transport_in_t, kind), 0, { 0 } },
    { NB_CODEC_JSON_STRING, "bind_host", offsetof(cfg_transport_in_t, bind_host), 0, { 0 } },
    { NB_CODEC_JSON_U32, "data_port", offsetof(cfg_transport_in_t, data_port), 0, { 0 } },
    { NB_CODEC_JSON_U32, "frag_bytes", offsetof(cfg_transport_in_t, frag_bytes), 0, { 0 } },
    { NB_CODEC_JSON_U32, "max_msg_bytes", offsetof(cfg_transport_in_t, max_msg_bytes), 0, { 0 } },
    { NB_CODEC_JSON_U32,
      "backpressure_bytes",
      offsetof(cfg_transport_in_t, backpressure_bytes),
      0,
      { 0 } },
    { NB_CODEC_JSON_U32,
      "rtcp_interval_ms",
      offsetof(cfg_transport_in_t, rtcp_interval_ms),
      0,
      { 0 } },
    { NB_CODEC_JSON_BOOL,
      "rtcp",
      offsetof(cfg_transport_in_t, rtcp),
      offsetof(cfg_transport_in_t, rtcp_present),
      { 0 } },
};

static const nb_codec_json_model_t g_discovery_model[] = {
    { NB_CODEC_JSON_OBJ, NULL, 0, 0, { .obj_size = sizeof(cfg_discovery_in_t) } },
    { NB_CODEC_JSON_STRING, "mode", offsetof(cfg_discovery_in_t, mode), 0, { 0 } },
    { NB_CODEC_JSON_STRING, "broadcast", offsetof(cfg_discovery_in_t, broadcast), 0, { 0 } },
    { NB_CODEC_JSON_U32,
      "hello_interval_ms",
      offsetof(cfg_discovery_in_t, hello_interval_ms),
      0,
      { 0 } },
    { NB_CODEC_JSON_U32, "node_ttl_ms", offsetof(cfg_discovery_in_t, node_ttl_ms), 0, { 0 } },
};

static const nb_codec_json_model_t g_metrics_model[] = {
    { NB_CODEC_JSON_OBJ, NULL, 0, 0, { .obj_size = sizeof(cfg_metrics_in_t) } },
    { NB_CODEC_JSON_BOOL, "disabled", offsetof(cfg_metrics_in_t, disabled), 0, { 0 } },
    { NB_CODEC_JSON_BOOL,
      "latency_disabled",
      offsetof(cfg_metrics_in_t, latency_disabled),
      0,
      { 0 } },
    { NB_CODEC_JSON_U32, "sample_every", offsetof(cfg_metrics_in_t, sample_every), 0, { 0 } },
    { NB_CODEC_JSON_BOOL,
      "per_peer_disabled",
      offsetof(cfg_metrics_in_t, per_peer_disabled),
      0,
      { 0 } },
};

static const nb_codec_json_model_t g_log_model[] = {
    { NB_CODEC_JSON_OBJ, NULL, 0, 0, { .obj_size = sizeof(cfg_log_in_t) } },
    { NB_CODEC_JSON_BOOL, "disabled", offsetof(cfg_log_in_t, disabled), 0, { 0 } },
    { NB_CODEC_JSON_STRING, "level", offsetof(cfg_log_in_t, level), 0, { 0 } },
    { NB_CODEC_JSON_STRING, "sink", offsetof(cfg_log_in_t, sink), 0, { 0 } },
    { NB_CODEC_JSON_STRING, "file", offsetof(cfg_log_in_t, file), 0, { 0 } },
};

static const nb_codec_json_model_t g_route_model[] = {
    { NB_CODEC_JSON_OBJ, NULL, 0, 0, { .obj_size = sizeof(cfg_route_in_t) } },
    { NB_CODEC_JSON_STRING, "service", offsetof(cfg_route_in_t, service), 0, { 0 } },
    { NB_CODEC_JSON_STRING, "endpoint", offsetof(cfg_route_in_t, endpoint), 0, { 0 } },
};

static const nb_codec_json_model_t g_binding_model[] = {
    { NB_CODEC_JSON_OBJ, NULL, 0, 0, { .obj_size = sizeof(cfg_binding_in_t) } },
    { NB_CODEC_JSON_STRING, "name", offsetof(cfg_binding_in_t, name), 0, { 0 } },
    { NB_CODEC_JSON_STRING, "endpoint", offsetof(cfg_binding_in_t, endpoint), 0, { 0 } },
};

#define CFG_MODEL_N(model_) (sizeof(model_) / sizeof((model_)[0]))

/* ---- 小工具 ---- */

/* 复制字符串到定长缓冲；src 为空时使用 def；超容量返回 ERR_INVALID。 */
static int cfg_copy(char *dst, size_t cap, const char *src, const char *def)
{
    int n;
    if (!src) {
        src = def;
    }
    if (!src) {
        if (cap) {
            dst[0] = '\0';
        }
        return OK;
    }
    n = snprintf(dst, cap, "%s", src);
    if (n < 0 || (size_t)n >= cap) {
        return ERR_INVALID;
    }
    return OK;
}

/* 堆复制字符串；失败返回 NULL。 */
static char *cfg_dup(const char *src)
{
    size_t n;
    char *copy;
    if (!src) {
        return NULL;
    }
    n = strlen(src);
    copy = (char *)malloc(n + 1u);
    if (copy) {
        memcpy(copy, src, n + 1u);
    }
    return copy;
}

/* 把子对象填充进已清零的中间结构；obj 为 NULL 时保持默认。 */
static int cfg_fill(const cJSON *obj, const nb_codec_json_model_t *model, size_t n, void *out)
{
    if (!obj) {
        return OK;
    }
    return nb_codec_json_fill_object(obj, model, n, out) == NB_CODEC_OK ? OK : ERR_INVALID;
}

/* 取可选子对象：不存在返回 OK+NULL；存在但类型不对返回 ERR_INVALID。 */
static int cfg_child(const cJSON *root, const char *key, const cJSON **out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    *out = NULL;
    if (!item) {
        return OK;
    }
    if (!cJSON_IsObject(item)) {
        return ERR_INVALID;
    }
    *out = item;
    return OK;
}

/* 读取顶层可选 u32（旧扁平格式），校验整数与范围。 */
static int cfg_legacy_u32(
    const cJSON *root, const char *key, uint32_t min, uint32_t max, uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    double d;
    uint32_t v;
    if (!item) {
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

/* 复制顶层可选字符串（旧扁平格式）为堆字符串。 */
static int cfg_legacy_str(const cJSON *root, const char *key, char **out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    size_t n;
    char *copy;
    if (!item) {
        return OK;
    }
    if (!cJSON_IsString(item) || !item->valuestring) {
        return ERR_INVALID;
    }
    n = strlen(item->valuestring);
    copy = (char *)malloc(n + 1u);
    if (!copy) {
        return ERR_NOMEM;
    }
    memcpy(copy, item->valuestring, n + 1u);
    free(*out);
    *out = copy;
    return OK;
}

/* 复制顶层可选 bool（旧扁平格式）到 present/flag。 */
static int cfg_legacy_bool(const cJSON *root, const char *key, uint8_t *flag, uint8_t *present)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!item) {
        return OK;
    }
    if (!cJSON_IsBool(item)) {
        return ERR_INVALID;
    }
    *flag = cJSON_IsTrue(item) ? 1u : 0u;
    if (present) {
        *present = 1u;
    }
    return OK;
}

static int cfg_transport_kind(const char *text, transport_kind_t *out)
{
    if (!text) {
        return ERR_INVALID;
    }
    if (strcmp(text, "tcp") == 0 || strcmp(text, "TCP") == 0) {
        *out = TRANSPORT_TCP;
        return OK;
    }
    if (strcmp(text, "udp") == 0 || strcmp(text, "UDP") == 0) {
        *out = TRANSPORT_UDP;
        return OK;
    }
    return ERR_INVALID;
}

static int cfg_mode(const char *text, node_mode_t *out)
{
    if (!text) {
        return ERR_INVALID;
    }
    if (strcmp(text, "static") == 0 || strcmp(text, "STATIC") == 0) {
        *out = MODE_STATIC;
        return OK;
    }
    if (strcmp(text, "mesh") == 0 || strcmp(text, "MESH") == 0) {
        *out = MODE_MESH;
        return OK;
    }
    return ERR_INVALID;
}

/* discovery.mode：mesh=开启发现，static/off/none=关闭。 */
static int cfg_discovery_mode(const char *text, int *disabled)
{
    if (!text) {
        return ERR_INVALID;
    }
    if (strcmp(text, "mesh") == 0 || strcmp(text, "MESH") == 0 || strcmp(text, "on") == 0) {
        *disabled = 0;
        return OK;
    }
    if (strcmp(text, "static") == 0 || strcmp(text, "off") == 0 || strcmp(text, "none") == 0 ||
        strcmp(text, "disabled") == 0) {
        *disabled = 1;
        return OK;
    }
    return ERR_INVALID;
}

static int cfg_log_level(const char *text, uint8_t *out)
{
    static const struct {
        const char *name;
        uint8_t level;
    } levels[] = {
        { "assert", NODE_LOG_ASSERT },
        { "error", NODE_LOG_ERROR },
        { "warn", NODE_LOG_WARN },
        { "info", NODE_LOG_INFO },
        { "debug", NODE_LOG_DEBUG },
        { "verbose", NODE_LOG_VERBOSE },
    };
    size_t i;
    if (!text) {
        return ERR_INVALID;
    }
    for (i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        if (strcmp(text, levels[i].name) == 0) {
            *out = levels[i].level;
            return OK;
        }
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
    if (cfg_copy(host, host_cap, endpoint.host, NULL) != OK) {
        return ERR_INVALID;
    }
    *port = endpoint.port;
    return OK;
}

/* ---- 子结构装载 ---- */

static int load_runtime(const cJSON *root, cfg_runtime_in_t *in)
{
    const cJSON *obj = NULL;
    if (cfg_child(root, "runtime", &obj) != OK) {
        return ERR_INVALID;
    }
    if (obj) {
        return cfg_fill(obj, g_runtime_model, CFG_MODEL_N(g_runtime_model), in);
    }
    /* 旧扁平格式：字段在顶层。 */
    if (cfg_legacy_u32(root, "io_threads", 0, 1024, &in->io_threads) != OK ||
        cfg_legacy_u32(root, "handler_threads", 0, 1024, &in->handler_threads) != OK ||
        cfg_legacy_u32(root,
                       "handler_max_concurrency",
                       0,
                       UINT32_MAX,
                       &in->handler_max_concurrency) != OK ||
        cfg_legacy_u32(root, "max_inflight", 0, UINT32_MAX, &in->max_inflight) != OK ||
        cfg_legacy_u32(root, "max_streams", 0, UINT32_MAX, &in->max_streams) != OK ||
        cfg_legacy_u32(root, "peer_rate_limit_pps", 0, UINT32_MAX, &in->peer_rate_limit_pps) !=
            OK) {
        return ERR_INVALID;
    }
    return OK;
}

static int load_transport(const cJSON *root, cfg_transport_in_t *in, const cJSON **bindings)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "transport");
    *bindings = NULL;
    if (item && cJSON_IsObject(item)) {
        if (cfg_fill(item, g_transport_model, CFG_MODEL_N(g_transport_model), in) != OK) {
            return ERR_INVALID;
        }
        *bindings = cJSON_GetObjectItemCaseSensitive(item, "bindings");
    } else {
        if (item) {
            if (!cJSON_IsString(item) || !item->valuestring) {
                return ERR_INVALID;
            }
            if (cfg_legacy_str(root, "transport", &in->kind) != OK) {
                return ERR_INVALID;
            }
        }
        if (cfg_legacy_str(root, "bind_host", &in->bind_host) != OK ||
            cfg_legacy_u32(root, "data_port", 0, UINT16_MAX, &in->data_port) != OK ||
            cfg_legacy_u32(root, "frag_bytes", 0, UINT16_MAX, &in->frag_bytes) != OK ||
            cfg_legacy_u32(root, "max_msg_bytes", 0, UINT32_MAX, &in->max_msg_bytes) != OK ||
            cfg_legacy_u32(root,
                           "backpressure_bytes",
                           0,
                           UINT32_MAX,
                           &in->backpressure_bytes) != OK ||
            cfg_legacy_u32(root, "rtcp_interval_ms", 0, UINT32_MAX, &in->rtcp_interval_ms) != OK ||
            cfg_legacy_bool(root, "rtcp", &in->rtcp, &in->rtcp_present) != OK) {
            return ERR_INVALID;
        }
        *bindings = cJSON_GetObjectItemCaseSensitive(root, "bindings");
    }
    if (*bindings && !cJSON_IsArray(*bindings)) {
        return ERR_INVALID;
    }
    return OK;
}

static int load_discovery(const cJSON *root, cfg_discovery_in_t *in, const cJSON **peers)
{
    const cJSON *obj = NULL;
    *peers = NULL;
    if (cfg_child(root, "discovery", &obj) != OK) {
        return ERR_INVALID;
    }
    if (obj) {
        if (cfg_fill(obj, g_discovery_model, CFG_MODEL_N(g_discovery_model), in) != OK) {
            return ERR_INVALID;
        }
        *peers = cJSON_GetObjectItemCaseSensitive(obj, "peers");
    }
    if (*peers && !cJSON_IsArray(*peers)) {
        return ERR_INVALID;
    }
    return OK;
}

static int load_metrics(const cJSON *root, cfg_metrics_in_t *in)
{
    const cJSON *obj = NULL;
    if (cfg_child(root, "metrics", &obj) != OK) {
        return ERR_INVALID;
    }
    return cfg_fill(obj, g_metrics_model, CFG_MODEL_N(g_metrics_model), in);
}

static int load_log(const cJSON *root, cfg_log_in_t *in)
{
    const cJSON *obj = NULL;
    if (cfg_child(root, "log", &obj) != OK) {
        return ERR_INVALID;
    }
    return cfg_fill(obj, g_log_model, CFG_MODEL_N(g_log_model), in);
}

/* ---- 转换到 config_t ---- */

static int fill_bindings(config_t *out, const cJSON *bindings)
{
    const cJSON *item;
    if (!bindings) {
        return OK;
    }
    cJSON_ArrayForEach(item, bindings)
    {
        binding_endpoint_t *binding;
        binding_config_t *view;
        cfg_binding_in_t parsed;
        const char *endpoint;
        const char *name = NULL;
        endpoint_t endpoint_parsed;

        if (out->binding_count >= CONFIG_MAX_BINDINGS) {
            return ERR_INVALID;
        }
        memset(&parsed, 0, sizeof(parsed));
        if (cJSON_IsString(item) && item->valuestring) {
            parsed.endpoint = cfg_dup(item->valuestring);
            if (!parsed.endpoint) {
                return ERR_INVALID;
            }
        } else if (cJSON_IsObject(item)) {
            if (cfg_fill(item, g_binding_model, CFG_MODEL_N(g_binding_model), &parsed) != OK) {
                return ERR_INVALID;
            }
            if (!parsed.endpoint) {
                const cJSON *uri = cJSON_GetObjectItemCaseSensitive(item, "uri");
                if (uri && cJSON_IsString(uri) && uri->valuestring) {
                    parsed.endpoint = cfg_dup(uri->valuestring);
                    if (!parsed.endpoint) {
                        return ERR_INVALID;
                    }
                }
            }
        } else {
            return ERR_INVALID;
        }
        endpoint = parsed.endpoint;
        name = parsed.name;
        if (!endpoint || endpoint_parse(endpoint, &endpoint_parsed) != OK) {
            nb_codec_json_clear(&parsed, g_binding_model, CFG_MODEL_N(g_binding_model));
            return ERR_INVALID;
        }
        binding = &out->bindings[out->binding_count];
        view = &out->binding_configs[out->binding_count];
        if (cfg_copy(binding->endpoint, sizeof(binding->endpoint), endpoint, NULL) != OK) {
            nb_codec_json_clear(&parsed, g_binding_model, CFG_MODEL_N(g_binding_model));
            return ERR_INVALID;
        }
        if (name && name[0]) {
            if (cfg_copy(binding->name, sizeof(binding->name), name, NULL) != OK) {
                nb_codec_json_clear(&parsed, g_binding_model, CFG_MODEL_N(g_binding_model));
                return ERR_INVALID;
            }
        } else {
            if (cfg_copy(binding->name, sizeof(binding->name), endpoint_parsed.scheme, NULL) !=
                OK) {
                nb_codec_json_clear(&parsed, g_binding_model, CFG_MODEL_N(g_binding_model));
                return ERR_INVALID;
            }
        }
        if (!binding->name[0]) {
            nb_codec_json_clear(&parsed, g_binding_model, CFG_MODEL_N(g_binding_model));
            return ERR_INVALID;
        }
        view->name = binding->name;
        view->endpoint = binding->endpoint;
        out->binding_count++;
        nb_codec_json_clear(&parsed, g_binding_model, CFG_MODEL_N(g_binding_model));
    }
    return OK;
}

static int fill_peers(config_t *out, const cJSON *peers)
{
    const cJSON *item;
    if (!peers) {
        return OK;
    }
    cJSON_ArrayForEach(item, peers)
    {
        peer_endpoint_t *peer;
        if (out->peer_count >= CONFIG_MAX_PEERS) {
            return ERR_INVALID;
        }
        if (!cJSON_IsString(item) || !item->valuestring) {
            return ERR_INVALID;
        }
        peer = &out->peers[out->peer_count];
        if (cfg_host_port(item->valuestring, peer->ip, sizeof(peer->ip), &peer->port) != OK) {
            return ERR_INVALID;
        }
        if (!peer->ip[0] || !peer->port) {
            return ERR_INVALID;
        }
        out->peer_count++;
    }
    return OK;
}

static int fill_routes(config_t *out, const cJSON *routes)
{
    const cJSON *item;
    if (!routes) {
        return OK;
    }
    if (!cJSON_IsArray(routes)) {
        return ERR_INVALID;
    }
    cJSON_ArrayForEach(item, routes)
    {
        static_route_t *route;
        cfg_route_in_t parsed;
        endpoint_t endpoint;
        if (out->route_count >= CONFIG_MAX_ROUTES) {
            return ERR_INVALID;
        }
        if (!cJSON_IsObject(item)) {
            return ERR_INVALID;
        }
        memset(&parsed, 0, sizeof(parsed));
        if (cfg_fill(item, g_route_model, CFG_MODEL_N(g_route_model), &parsed) != OK) {
            return ERR_INVALID;
        }
        route = &out->routes[out->route_count];
        if (cfg_copy(route->service, sizeof(route->service), parsed.service, NULL) != OK ||
            cfg_copy(route->endpoint, sizeof(route->endpoint), parsed.endpoint, NULL) != OK ||
            !route->service[0] || !route->endpoint[0] ||
            endpoint_parse(route->endpoint, &endpoint) != OK) {
            nb_codec_json_clear(&parsed, g_route_model, CFG_MODEL_N(g_route_model));
            return ERR_INVALID;
        }
        out->route_count++;
        nb_codec_json_clear(&parsed, g_route_model, CFG_MODEL_N(g_route_model));
    }
    return OK;
}

/* 把中间结构合入 config_t.node，应用默认值并校验范围。 */
static int apply_config(config_t *out,
                        const cfg_runtime_in_t *rt,
                        const cfg_transport_in_t *tp,
                        const cfg_discovery_in_t *disc,
                        const cfg_metrics_in_t *mt,
                        const cfg_log_in_t *lg,
                        const cJSON *root,
                        const cJSON *bindings,
                        const cJSON *peers,
                        const cJSON *routes)
{
    node_config_t *node = &out->node;
    node_mode_t mode = MODE_MESH;
    transport_kind_t kind = TRANSPORT_UDP;
    const char *name = NULL;
    const cJSON *item;

    node_config_default(node);

    /* name / mode（顶层，两种格式一致） */
    item = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (item) {
        if (!cJSON_IsString(item) || !item->valuestring) {
            return ERR_INVALID;
        }
        name = item->valuestring;
    }
    if (cfg_copy(out->name, sizeof(out->name), name, ZRI_DEFAULT_NODE_NAME) != OK) {
        return ERR_INVALID;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "mode");
    if (item) {
        if (!cJSON_IsString(item) || cfg_mode(item->valuestring, &mode) != OK) {
            return ERR_INVALID;
        }
    }
    node->mode = mode;
    node->name = out->name;

    /* runtime */
    if (rt->io_threads > ZRI_MAX_IO_THREADS || rt->handler_threads > ZRI_MAX_HANDLER_THREADS) {
        return ERR_INVALID;
    }
    node->runtime.io_threads = (unsigned)rt->io_threads;
    node->runtime.handler_threads = (unsigned)rt->handler_threads;
    node->runtime.handler_max_concurrency = (unsigned)rt->handler_max_concurrency;
    node->runtime.max_inflight = rt->max_inflight;
    node->runtime.max_streams = rt->max_streams;
    node->runtime.peer_rate_limit_pps = rt->peer_rate_limit_pps;

    /* transport */
    if (tp->kind && cfg_transport_kind(tp->kind, &kind) != OK) {
        return ERR_INVALID;
    }
    if (tp->data_port > UINT16_MAX || tp->frag_bytes > UINT16_MAX) {
        return ERR_INVALID;
    }
    node->transport.transport = kind;
    if (cfg_copy(out->bind_host, sizeof(out->bind_host), tp->bind_host, ZRI_DEFAULT_BIND_HOST) !=
        OK) {
        return ERR_INVALID;
    }
    node->transport.bind_host = out->bind_host;
    node->transport.data_port = (uint16_t)tp->data_port;
    node->transport.frag_bytes = (uint16_t)tp->frag_bytes;
    node->transport.max_msg_bytes = tp->max_msg_bytes;
    node->transport.backpressure_bytes = tp->backpressure_bytes;
    node->transport.rtcp_interval_ms = tp->rtcp_interval_ms;
    node->transport.rtcp_disabled = tp->rtcp_present ? (tp->rtcp ? 0 : 1) : 0;

    /* discovery：broadcast（新）或 discovery_host/port（旧）二选一 */
    if (disc->mode && cfg_discovery_mode(disc->mode, &node->discovery.disabled) != OK) {
        return ERR_INVALID;
    }
    if (disc->broadcast) {
        if (cfg_host_port(
                disc->broadcast, out->discovery_host, sizeof(out->discovery_host), &node->discovery.port) !=
            OK) {
            return ERR_INVALID;
        }
    } else {
        const cJSON *host = cJSON_GetObjectItemCaseSensitive(root, "discovery_host");
        uint32_t port = 0;
        if (host && (!cJSON_IsString(host) || !host->valuestring)) {
            return ERR_INVALID;
        }
        if (cfg_legacy_u32(root, "discovery_port", 0, UINT16_MAX, &port) != OK) {
            return ERR_INVALID;
        }
        if (cfg_copy(out->discovery_host,
                     sizeof(out->discovery_host),
                     host ? host->valuestring : NULL,
                     ZRI_DEFAULT_DISCOVERY_HOST) != OK) {
            return ERR_INVALID;
        }
        node->discovery.port = (uint16_t)port;
    }
    node->discovery.host = out->discovery_host;
    node->discovery.hello_interval_ms = disc->hello_interval_ms;
    node->discovery.node_ttl_ms = disc->node_ttl_ms;

    /* metrics */
    node->metrics.disabled = mt->disabled;
    node->metrics.latency_disabled = mt->latency_disabled;
    node->metrics.sample_every =
        mt->sample_every ? mt->sample_every : ZRI_DEFAULT_METRICS_SAMPLE_EVERY;
    node->metrics.per_peer_disabled = mt->per_peer_disabled;

    /* log */
    node->log.disabled = lg->disabled;
    if (lg->level && cfg_log_level(lg->level, &node->log.level) != OK) {
        return ERR_INVALID;
    }
    if (cfg_copy(out->log_file, sizeof(out->log_file), lg->file, NULL) != OK) {
        return ERR_INVALID;
    }
    node->log.file = lg->file ? out->log_file : NULL;

    if (fill_bindings(out, bindings) != OK || fill_peers(out, peers) != OK ||
        fill_routes(out, routes) != OK) {
        return ERR_INVALID;
    }
    if (out->binding_count > 0) {
        node->transport.bindings = out->binding_configs;
        node->transport.binding_count = out->binding_count;
    }
    return OK;
}

/* ---- 配置加载 ---- */

int config_load(const char *json, config_t *out)
{
    cJSON *root = NULL;
    cfg_runtime_in_t rt;
    cfg_transport_in_t tp;
    cfg_discovery_in_t disc;
    cfg_metrics_in_t mt;
    cfg_log_in_t lg;
    const cJSON *bindings = NULL;
    const cJSON *peers = NULL;
    const cJSON *routes = NULL;
    int rc = ERR_INVALID;

    if (!json || !out) {
        return ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    memset(&rt, 0, sizeof(rt));
    memset(&tp, 0, sizeof(tp));
    memset(&disc, 0, sizeof(disc));
    memset(&mt, 0, sizeof(mt));
    memset(&lg, 0, sizeof(lg));

    root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        goto done;
    }
    routes = cJSON_GetObjectItemCaseSensitive(root, "routes");
    if (routes && !cJSON_IsArray(routes)) {
        goto done;
    }
    if (load_runtime(root, &rt) != OK || load_transport(root, &tp, &bindings) != OK ||
        load_discovery(root, &disc, &peers) != OK || load_metrics(root, &mt) != OK ||
        load_log(root, &lg) != OK) {
        goto done;
    }
    if (apply_config(out, &rt, &tp, &disc, &mt, &lg, root, bindings, peers, routes) != OK) {
        memset(out, 0, sizeof(*out));
        goto done;
    }
    rc = OK;

done:
    nb_codec_json_clear(&rt, g_runtime_model, CFG_MODEL_N(g_runtime_model));
    nb_codec_json_clear(&tp, g_transport_model, CFG_MODEL_N(g_transport_model));
    nb_codec_json_clear(&disc, g_discovery_model, CFG_MODEL_N(g_discovery_model));
    nb_codec_json_clear(&mt, g_metrics_model, CFG_MODEL_N(g_metrics_model));
    nb_codec_json_clear(&lg, g_log_model, CFG_MODEL_N(g_log_model));
    if (root) {
        cJSON_Delete(root);
    }
    return rc;
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
