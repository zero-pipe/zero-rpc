/*
 * 配置加载：模型驱动的 JSON 解析。
 *
 * 配置按模块拆成子对象（runtime / transport / discovery / metrics / log），
 * 由 codec_json 的 model 描述字段布局后统一填充；缺省值由 defaults.h 与
 * nb_node_config_normalize 收敛，未知字段忽略，已知字段非法时返回带路径的错误。
 */
#include <config.h>
#include <node.h>

#include "cJSON.h"
#include "codec_json_internal.h"
#include "defaults.h"

#include <ntk.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int nb_cfg_fail_code(nb_config_error_t *err, int code, const char *path, const char *reason)
{
    if (err) {
        err->code = code;
        snprintf(err->path, sizeof(err->path), "%s", path ? path : "");
        snprintf(err->reason, sizeof(err->reason), "%s", reason ? reason : "");
    }
    return code;
}

static int nb_cfg_fail(nb_config_error_t *err, const char *path, const char *reason)
{
    return nb_cfg_fail_code(err, NB_ERR_INVALID, path, reason);
}

static const char *nb_cfg_codec_reason(int status)
{
    switch (status) {
    case NB_CODEC_TYPE_MISMATCH:
        return "type mismatch";
    case NB_CODEC_LIMIT_EXCEEDED:
        return "value out of range";
    case NB_CODEC_MALFORMED:
        return "malformed value";
    case NB_CODEC_OUT_OF_MEMORY:
        return "out of memory";
    case NB_CODEC_INVALID_ARGUMENT:
        return "invalid argument";
    default:
        return "invalid value";
    }
}

/* ---- 默认值 ---- */

void nb_node_runtime_config_default(nb_node_runtime_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
}

void nb_node_transport_config_default(nb_node_transport_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->transport = NB_TRANSPORT_UDP;
    cfg->bind_host = NB_DEFAULT_BIND_HOST;
}

void nb_node_discovery_config_default(nb_node_discovery_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->host = NB_DEFAULT_DISCOVERY_HOST;
}

void nb_node_metrics_config_default(nb_node_metrics_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->sample_every = NB_DEFAULT_METRICS_SAMPLE_EVERY;
}

void nb_node_log_config_default(nb_node_log_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->level = NB_NODE_LOG_INFO;
}

void nb_node_config_default(nb_node_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->name = NB_DEFAULT_NODE_NAME;
    cfg->mode = NB_MODE_MESH;
    nb_node_runtime_config_default(&cfg->runtime);
    nb_node_transport_config_default(&cfg->transport);
    nb_node_discovery_config_default(&cfg->discovery);
    nb_node_metrics_config_default(&cfg->metrics);
    nb_node_log_config_default(&cfg->log);
}

void nb_node_config_normalize(nb_node_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    if (!cfg->name) {
        cfg->name = NB_DEFAULT_NODE_NAME;
    }
    if (!cfg->transport.bind_host) {
        cfg->transport.bind_host = NB_DEFAULT_BIND_HOST;
    }
    if (!cfg->discovery.host) {
        cfg->discovery.host = NB_DEFAULT_DISCOVERY_HOST;
    }
    if (!cfg->runtime.io_threads) {
        unsigned hw = ntk_thread_hardware_concurrency();
        cfg->runtime.io_threads = hw ? hw : 1u;
    }
    if (cfg->runtime.io_threads > IO_MAX) {
        cfg->runtime.io_threads = IO_MAX;
    }
    if (!cfg->transport.frag_bytes || cfg->transport.frag_bytes < NB_DEFAULT_FRAG_MIN) {
        cfg->transport.frag_bytes = NB_DEFAULT_FRAG_BYTES;
    } else if (cfg->transport.frag_bytes > NB_DEFAULT_FRAG_MAX) {
        cfg->transport.frag_bytes = NB_DEFAULT_FRAG_MAX;
    }
    if (!cfg->transport.max_msg_bytes) {
        cfg->transport.max_msg_bytes = NB_DEFAULT_MAX_MSG;
    } else if (cfg->transport.max_msg_bytes > MAX_MSG_LIMIT) {
        cfg->transport.max_msg_bytes = MAX_MSG_LIMIT;
    }
    if (cfg->transport.max_msg_bytes < cfg->transport.frag_bytes) {
        cfg->transport.max_msg_bytes = cfg->transport.frag_bytes;
    }
    if (!cfg->transport.backpressure_bytes) {
        cfg->transport.backpressure_bytes = NB_DEFAULT_BACKPRESSURE_BYTES;
    }
    if (!cfg->transport.rtcp_interval_ms) {
        cfg->transport.rtcp_interval_ms = NB_DEFAULT_RTCP_INTERVAL_MS;
    }
    if (!cfg->discovery.port) {
        cfg->discovery.port = NB_DEFAULT_DISCOVERY_PORT;
    }
    if (!cfg->discovery.hello_interval_ms) {
        cfg->discovery.hello_interval_ms = NB_DEFAULT_HELLO_INTERVAL_MS;
    }
    if (!cfg->discovery.node_ttl_ms) {
        cfg->discovery.node_ttl_ms = NB_DEFAULT_NODE_TTL_MS;
    }
    if (!cfg->runtime.max_inflight) {
        cfg->runtime.max_inflight = NB_DEFAULT_MAX_INFLIGHT;
    }
    if (!cfg->runtime.max_streams) {
        cfg->runtime.max_streams = NB_DEFAULT_MAX_STREAMS;
    } else if (cfg->runtime.max_streams > MAX_RECV_STREAMS) {
        cfg->runtime.max_streams = MAX_RECV_STREAMS;
    }
    if (!cfg->metrics.sample_every) {
        cfg->metrics.sample_every = NB_DEFAULT_METRICS_SAMPLE_EVERY;
    }
    /* log.level: 0 是合法的 NB_NODE_LOG_ASSERT，不能当作“未设置”。 */
}

/* ---- 模型驱动的解析中间结构 ---- */

typedef struct nb_cfg_runtime_in {
    uint32_t io_threads;
    uint32_t handler_threads;
    uint32_t handler_max_concurrency;
    uint32_t max_inflight;
    uint32_t max_streams;
    uint32_t peer_rate_limit_pps;
} nb_cfg_runtime_in_t;

typedef struct nb_cfg_transport_in {
    char *kind;
    char *bind_host;
    uint32_t data_port;
    uint32_t frag_bytes;
    uint32_t max_msg_bytes;
    uint32_t backpressure_bytes;
    uint32_t rtcp_interval_ms;
    uint8_t rtcp;
    uint8_t rtcp_present;
} nb_cfg_transport_in_t;

typedef struct nb_cfg_discovery_in {
    char *mode;
    char *broadcast;
    uint32_t hello_interval_ms;
    uint32_t node_ttl_ms;
} nb_cfg_discovery_in_t;

typedef struct nb_cfg_metrics_in {
    uint8_t disabled;
    uint8_t latency_disabled;
    uint32_t sample_every;
    uint8_t per_peer_disabled;
} nb_cfg_metrics_in_t;

typedef struct nb_cfg_log_in {
    uint8_t disabled;
    char *level;
    char *sink;
    char *file;
} nb_cfg_log_in_t;

typedef struct nb_cfg_route_in {
    char *service;
    char *endpoint;
} nb_cfg_route_in_t;

typedef struct nb_cfg_binding_in {
    char *name;
    char *endpoint;
} nb_cfg_binding_in_t;

/* 用显式结构体字段偏移描述每个子对象，交给 codec_json 填充。 */
static const nb_codec_json_model_t nb_g_runtime_model[] = {
    { NB_CODEC_JSON_OBJ, NULL, 0, 0, { .obj_size = sizeof(nb_cfg_runtime_in_t) } },
    { NB_CODEC_JSON_U32, "io_threads", offsetof(nb_cfg_runtime_in_t, io_threads), 0, { 0 } },
    { NB_CODEC_JSON_U32, "handler_threads", offsetof(nb_cfg_runtime_in_t, handler_threads), 0, { 0 } },
    { NB_CODEC_JSON_U32,
      "handler_max_concurrency",
      offsetof(nb_cfg_runtime_in_t, handler_max_concurrency),
      0,
      { 0 } },
    { NB_CODEC_JSON_U32, "max_inflight", offsetof(nb_cfg_runtime_in_t, max_inflight), 0, { 0 } },
    { NB_CODEC_JSON_U32, "max_streams", offsetof(nb_cfg_runtime_in_t, max_streams), 0, { 0 } },
    { NB_CODEC_JSON_U32,
      "peer_rate_limit_pps",
      offsetof(nb_cfg_runtime_in_t, peer_rate_limit_pps),
      0,
      { 0 } },
};

static const nb_codec_json_model_t nb_g_transport_model[] = {
    { NB_CODEC_JSON_OBJ, NULL, 0, 0, { .obj_size = sizeof(nb_cfg_transport_in_t) } },
    { NB_CODEC_JSON_STRING, "kind", offsetof(nb_cfg_transport_in_t, kind), 0, { 0 } },
    { NB_CODEC_JSON_STRING, "bind_host", offsetof(nb_cfg_transport_in_t, bind_host), 0, { 0 } },
    { NB_CODEC_JSON_U32, "data_port", offsetof(nb_cfg_transport_in_t, data_port), 0, { 0 } },
    { NB_CODEC_JSON_U32, "frag_bytes", offsetof(nb_cfg_transport_in_t, frag_bytes), 0, { 0 } },
    { NB_CODEC_JSON_U32, "max_msg_bytes", offsetof(nb_cfg_transport_in_t, max_msg_bytes), 0, { 0 } },
    { NB_CODEC_JSON_U32,
      "backpressure_bytes",
      offsetof(nb_cfg_transport_in_t, backpressure_bytes),
      0,
      { 0 } },
    { NB_CODEC_JSON_U32,
      "rtcp_interval_ms",
      offsetof(nb_cfg_transport_in_t, rtcp_interval_ms),
      0,
      { 0 } },
    { NB_CODEC_JSON_BOOL,
      "rtcp",
      offsetof(nb_cfg_transport_in_t, rtcp),
      offsetof(nb_cfg_transport_in_t, rtcp_present),
      { 0 } },
};

static const nb_codec_json_model_t nb_g_discovery_model[] = {
    { NB_CODEC_JSON_OBJ, NULL, 0, 0, { .obj_size = sizeof(nb_cfg_discovery_in_t) } },
    { NB_CODEC_JSON_STRING, "mode", offsetof(nb_cfg_discovery_in_t, mode), 0, { 0 } },
    { NB_CODEC_JSON_STRING, "broadcast", offsetof(nb_cfg_discovery_in_t, broadcast), 0, { 0 } },
    { NB_CODEC_JSON_U32,
      "hello_interval_ms",
      offsetof(nb_cfg_discovery_in_t, hello_interval_ms),
      0,
      { 0 } },
    { NB_CODEC_JSON_U32, "node_ttl_ms", offsetof(nb_cfg_discovery_in_t, node_ttl_ms), 0, { 0 } },
};

static const nb_codec_json_model_t nb_g_metrics_model[] = {
    { NB_CODEC_JSON_OBJ, NULL, 0, 0, { .obj_size = sizeof(nb_cfg_metrics_in_t) } },
    { NB_CODEC_JSON_BOOL, "disabled", offsetof(nb_cfg_metrics_in_t, disabled), 0, { 0 } },
    { NB_CODEC_JSON_BOOL,
      "latency_disabled",
      offsetof(nb_cfg_metrics_in_t, latency_disabled),
      0,
      { 0 } },
    { NB_CODEC_JSON_U32, "sample_every", offsetof(nb_cfg_metrics_in_t, sample_every), 0, { 0 } },
    { NB_CODEC_JSON_BOOL,
      "per_peer_disabled",
      offsetof(nb_cfg_metrics_in_t, per_peer_disabled),
      0,
      { 0 } },
};

static const nb_codec_json_model_t nb_g_log_model[] = {
    { NB_CODEC_JSON_OBJ, NULL, 0, 0, { .obj_size = sizeof(nb_cfg_log_in_t) } },
    { NB_CODEC_JSON_BOOL, "disabled", offsetof(nb_cfg_log_in_t, disabled), 0, { 0 } },
    { NB_CODEC_JSON_STRING, "level", offsetof(nb_cfg_log_in_t, level), 0, { 0 } },
    { NB_CODEC_JSON_STRING, "sink", offsetof(nb_cfg_log_in_t, sink), 0, { 0 } },
    { NB_CODEC_JSON_STRING, "file", offsetof(nb_cfg_log_in_t, file), 0, { 0 } },
};

static const nb_codec_json_model_t nb_g_route_model[] = {
    { NB_CODEC_JSON_OBJ, NULL, 0, 0, { .obj_size = sizeof(nb_cfg_route_in_t) } },
    { NB_CODEC_JSON_STRING, "service", offsetof(nb_cfg_route_in_t, service), 0, { 0 } },
    { NB_CODEC_JSON_STRING, "endpoint", offsetof(nb_cfg_route_in_t, endpoint), 0, { 0 } },
};

static const nb_codec_json_model_t nb_g_binding_model[] = {
    { NB_CODEC_JSON_OBJ, NULL, 0, 0, { .obj_size = sizeof(nb_cfg_binding_in_t) } },
    { NB_CODEC_JSON_STRING, "name", offsetof(nb_cfg_binding_in_t, name), 0, { 0 } },
    { NB_CODEC_JSON_STRING, "endpoint", offsetof(nb_cfg_binding_in_t, endpoint), 0, { 0 } },
};

#define CFG_MODEL_N(model_) (sizeof(model_) / sizeof((model_)[0]))

/* ---- 小工具 ---- */

/* 复制字符串到定长缓冲；src 为空时使用 def；超容量返回 NB_ERR_INVALID。 */
static int nb_cfg_copy(char *dst, size_t cap, const char *src, const char *def)
{
    int n;
    if (!src) {
        src = def;
    }
    if (!src) {
        if (cap) {
            dst[0] = '\0';
        }
        return NB_OK;
    }
    n = snprintf(dst, cap, "%s", src);
    if (n < 0 || (size_t)n >= cap) {
        return NB_ERR_INVALID;
    }
    return NB_OK;
}

/* 堆复制字符串；失败返回 NULL。 */
static char *nb_cfg_dup(const char *src)
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
static int nb_cfg_fill(const cJSON *obj, const nb_codec_json_model_t *model, size_t n, void *out)
{
    if (!obj) {
        return NB_CODEC_OK;
    }
    return nb_codec_json_fill_object(obj, model, n, out);
}

/* 取可选子对象：不存在返回 NB_OK+NULL；存在但类型不对返回 NB_ERR_INVALID。 */
static int nb_cfg_child(const cJSON *root, const char *key, const cJSON **out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    *out = NULL;
    if (!item) {
        return NB_OK;
    }
    if (!cJSON_IsObject(item)) {
        return NB_ERR_INVALID;
    }
    *out = item;
    return NB_OK;
}

static int nb_cfg_transport_kind(const char *text, nb_transport_kind_t *out)
{
    if (!text) {
        return NB_ERR_INVALID;
    }
    if (strcmp(text, "tcp") == 0 || strcmp(text, "TCP") == 0) {
        *out = NB_TRANSPORT_TCP;
        return NB_OK;
    }
    if (strcmp(text, "udp") == 0 || strcmp(text, "UDP") == 0) {
        *out = NB_TRANSPORT_UDP;
        return NB_OK;
    }
    return NB_ERR_INVALID;
}

static int nb_cfg_mode(const char *text, nb_node_mode_t *out)
{
    if (!text) {
        return NB_ERR_INVALID;
    }
    if (strcmp(text, "static") == 0 || strcmp(text, "STATIC") == 0) {
        *out = NB_MODE_STATIC;
        return NB_OK;
    }
    if (strcmp(text, "mesh") == 0 || strcmp(text, "MESH") == 0) {
        *out = NB_MODE_MESH;
        return NB_OK;
    }
    return NB_ERR_INVALID;
}

/* discovery.mode：mesh=开启发现，static/off/none=关闭。 */
static int nb_cfg_discovery_mode(const char *text, int *disabled)
{
    if (!text) {
        return NB_ERR_INVALID;
    }
    if (strcmp(text, "mesh") == 0 || strcmp(text, "MESH") == 0 || strcmp(text, "on") == 0) {
        *disabled = 0;
        return NB_OK;
    }
    if (strcmp(text, "static") == 0 || strcmp(text, "off") == 0 || strcmp(text, "none") == 0 ||
        strcmp(text, "disabled") == 0) {
        *disabled = 1;
        return NB_OK;
    }
    return NB_ERR_INVALID;
}

static int nb_cfg_log_level(const char *text, uint8_t *out)
{
    static const struct {
        const char *name;
        uint8_t level;
    } levels[] = {
        { "assert", NB_NODE_LOG_ASSERT },
        { "error", NB_NODE_LOG_ERROR },
        { "warn", NB_NODE_LOG_WARN },
        { "info", NB_NODE_LOG_INFO },
        { "debug", NB_NODE_LOG_DEBUG },
        { "verbose", NB_NODE_LOG_VERBOSE },
    };
    size_t i;
    if (!text) {
        return NB_ERR_INVALID;
    }
    for (i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        if (strcmp(text, levels[i].name) == 0) {
            *out = levels[i].level;
            return NB_OK;
        }
    }
    return NB_ERR_INVALID;
}

/* 解析 "host:port"（含 [ipv6]:port）；成功返回 NB_OK。 */
static int nb_cfg_host_port(const char *text, char *host, size_t host_cap, uint16_t *port)
{
    nb_endpoint_t endpoint;
    char uri[NB_ENDPOINT_MAX * 2];
    int n;
    if (!text || !host || !port) {
        return NB_ERR_INVALID;
    }
    n = snprintf(uri, sizeof(uri), "udp://%s", text);
    if (n < 0 || (size_t)n >= sizeof(uri) || nb_endpoint_parse(uri, &endpoint) != NB_OK) {
        return NB_ERR_INVALID;
    }
    if (nb_cfg_copy(host, host_cap, endpoint.host, NULL) != NB_OK) {
        return NB_ERR_INVALID;
    }
    *port = endpoint.port;
    return NB_OK;
}

/* ---- 子结构装载 ---- */

static int nb_load_runtime(const cJSON *root, nb_cfg_runtime_in_t *in, nb_config_error_t *err)
{
    const cJSON *obj = NULL;
    int rc;

    if (nb_cfg_child(root, "runtime", &obj) != NB_OK) {
        return nb_cfg_fail(err, "runtime", "must be an object");
    }
    rc = nb_cfg_fill(obj, nb_g_runtime_model, CFG_MODEL_N(nb_g_runtime_model), in);
    return rc == NB_CODEC_OK ? NB_OK : nb_cfg_fail(err, "runtime", nb_cfg_codec_reason(rc));
}

static int nb_load_transport(const cJSON *root,
                          nb_cfg_transport_in_t *in,
                          const cJSON **bindings,
                          nb_config_error_t *err)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "transport");
    int rc;

    *bindings = NULL;
    if (!item) {
        return NB_OK;
    }
    if (!cJSON_IsObject(item)) {
        return nb_cfg_fail(err, "transport", "must be an object");
    }
    rc = nb_cfg_fill(item, nb_g_transport_model, CFG_MODEL_N(nb_g_transport_model), in);
    if (rc != NB_CODEC_OK) {
        return nb_cfg_fail(err, "transport", nb_cfg_codec_reason(rc));
    }
    *bindings = cJSON_GetObjectItemCaseSensitive(item, "bindings");
    if (*bindings && !cJSON_IsArray(*bindings)) {
        return nb_cfg_fail(err, "transport.bindings", "must be an array");
    }
    return NB_OK;
}

static int nb_load_discovery(const cJSON *root,
                          nb_cfg_discovery_in_t *in,
                          const cJSON **peers,
                          nb_config_error_t *err)
{
    const cJSON *obj = NULL;
    int rc;

    *peers = NULL;
    if (nb_cfg_child(root, "discovery", &obj) != NB_OK) {
        return nb_cfg_fail(err, "discovery", "must be an object");
    }
    rc = nb_cfg_fill(obj, nb_g_discovery_model, CFG_MODEL_N(nb_g_discovery_model), in);
    if (rc != NB_CODEC_OK) {
        return nb_cfg_fail(err, "discovery", nb_cfg_codec_reason(rc));
    }
    if (obj) {
        *peers = cJSON_GetObjectItemCaseSensitive(obj, "peers");
    }
    if (*peers && !cJSON_IsArray(*peers)) {
        return nb_cfg_fail(err, "discovery.peers", "must be an array");
    }
    return NB_OK;
}

static int nb_load_metrics(const cJSON *root, nb_cfg_metrics_in_t *in, nb_config_error_t *err)
{
    const cJSON *obj = NULL;
    int rc;

    if (nb_cfg_child(root, "metrics", &obj) != NB_OK) {
        return nb_cfg_fail(err, "metrics", "must be an object");
    }
    rc = nb_cfg_fill(obj, nb_g_metrics_model, CFG_MODEL_N(nb_g_metrics_model), in);
    return rc == NB_CODEC_OK ? NB_OK : nb_cfg_fail(err, "metrics", nb_cfg_codec_reason(rc));
}

static int nb_load_log(const cJSON *root, nb_cfg_log_in_t *in, nb_config_error_t *err)
{
    const cJSON *obj = NULL;
    int rc;

    if (nb_cfg_child(root, "log", &obj) != NB_OK) {
        return nb_cfg_fail(err, "log", "must be an object");
    }
    rc = nb_cfg_fill(obj, nb_g_log_model, CFG_MODEL_N(nb_g_log_model), in);
    return rc == NB_CODEC_OK ? NB_OK : nb_cfg_fail(err, "log", nb_cfg_codec_reason(rc));
}

/* ---- 转换到 nb_config_t ---- */

static int nb_fill_bindings(nb_config_t *out, const cJSON *bindings, nb_config_error_t *err)
{
    const cJSON *item;

    if (!bindings) {
        return NB_OK;
    }
    cJSON_ArrayForEach(item, bindings)
    {
        nb_binding_endpoint_t *binding;
        nb_binding_config_t *view;
        nb_cfg_binding_in_t parsed;
        const char *endpoint;
        const char *name;
        nb_endpoint_t endpoint_parsed;
        int rc = NB_OK;
        int frc;

        if (out->binding_count >= NB_CONFIG_MAX_BINDINGS) {
            return nb_cfg_fail(err, "transport.bindings", "too many bindings");
        }
        memset(&parsed, 0, sizeof(parsed));
        if (cJSON_IsString(item) && item->valuestring) {
            parsed.endpoint = nb_cfg_dup(item->valuestring);
            if (!parsed.endpoint) {
                rc = nb_cfg_fail(err, "transport.bindings", "out of memory");
                goto done;
            }
        } else if (cJSON_IsObject(item)) {
            frc = nb_cfg_fill(item, nb_g_binding_model, CFG_MODEL_N(nb_g_binding_model), &parsed);
            if (frc != NB_CODEC_OK) {
                rc = nb_cfg_fail(err, "transport.bindings", nb_cfg_codec_reason(frc));
                goto done;
            }
            if (!parsed.endpoint) {
                const cJSON *uri = cJSON_GetObjectItemCaseSensitive(item, "uri");
                if (uri && cJSON_IsString(uri) && uri->valuestring) {
                    parsed.endpoint = nb_cfg_dup(uri->valuestring);
                    if (!parsed.endpoint) {
                        rc = nb_cfg_fail(err, "transport.bindings", "out of memory");
                        goto done;
                    }
                }
            }
        } else {
            rc = nb_cfg_fail(err, "transport.bindings", "must be string or object");
            goto done;
        }
        endpoint = parsed.endpoint;
        name = parsed.name;
        if (!endpoint || nb_endpoint_parse(endpoint, &endpoint_parsed) != NB_OK) {
            rc = nb_cfg_fail(err, "transport.bindings", "invalid endpoint URI");
            goto done;
        }
        binding = &out->bindings[out->binding_count];
        view = &out->binding_configs[out->binding_count];
        if (nb_cfg_copy(binding->endpoint, sizeof(binding->endpoint), endpoint, NULL) != NB_OK) {
            rc = nb_cfg_fail(err, "transport.bindings", "endpoint too long");
            goto done;
        }
        if (name && name[0]) {
            if (nb_cfg_copy(binding->name, sizeof(binding->name), name, NULL) != NB_OK) {
                rc = nb_cfg_fail(err, "transport.bindings", "binding name too long");
                goto done;
            }
        } else if (nb_cfg_copy(binding->name, sizeof(binding->name), endpoint_parsed.scheme, NULL) !=
                   NB_OK) {
            rc = nb_cfg_fail(err, "transport.bindings", "binding name too long");
            goto done;
        }
        if (!binding->name[0]) {
            rc = nb_cfg_fail(err, "transport.bindings", "missing binding name");
            goto done;
        }
        view->name = binding->name;
        view->endpoint = binding->endpoint;
        out->binding_count++;
    done:
        nb_codec_json_clear(&parsed, nb_g_binding_model, CFG_MODEL_N(nb_g_binding_model));
        if (rc != NB_OK) {
            return rc;
        }
    }
    return NB_OK;
}

static int nb_fill_peers(nb_config_t *out, const cJSON *peers, nb_config_error_t *err)
{
    const cJSON *item;

    if (!peers) {
        return NB_OK;
    }
    cJSON_ArrayForEach(item, peers)
    {
        nb_peer_endpoint_t *peer;
        if (out->peer_count >= NB_CONFIG_MAX_PEERS) {
            return nb_cfg_fail(err, "discovery.peers", "too many peers");
        }
        if (!cJSON_IsString(item) || !item->valuestring) {
            return nb_cfg_fail(err, "discovery.peers", "must be a string");
        }
        peer = &out->peers[out->peer_count];
        if (nb_cfg_host_port(item->valuestring, peer->ip, sizeof(peer->ip), &peer->port) != NB_OK) {
            return nb_cfg_fail(err, "discovery.peers", "invalid host:port");
        }
        if (!peer->ip[0] || !peer->port) {
            return nb_cfg_fail(err, "discovery.peers", "invalid host:port");
        }
        out->peer_count++;
    }
    return NB_OK;
}

static int nb_fill_routes(nb_config_t *out, const cJSON *routes, nb_config_error_t *err)
{
    const cJSON *item;

    if (!routes) {
        return NB_OK;
    }
    if (!cJSON_IsArray(routes)) {
        return nb_cfg_fail(err, "routes", "must be an array");
    }
    cJSON_ArrayForEach(item, routes)
    {
        nb_static_route_t *route;
        nb_cfg_route_in_t parsed;
        nb_endpoint_t endpoint;
        int rc = NB_OK;
        int frc;

        if (out->route_count >= NB_CONFIG_MAX_ROUTES) {
            return nb_cfg_fail(err, "routes", "too many routes");
        }
        if (!cJSON_IsObject(item)) {
            return nb_cfg_fail(err, "routes", "each route must be an object");
        }
        memset(&parsed, 0, sizeof(parsed));
        frc = nb_cfg_fill(item, nb_g_route_model, CFG_MODEL_N(nb_g_route_model), &parsed);
        if (frc != NB_CODEC_OK) {
            nb_codec_json_clear(&parsed, nb_g_route_model, CFG_MODEL_N(nb_g_route_model));
            return nb_cfg_fail(err, "routes", nb_cfg_codec_reason(frc));
        }
        route = &out->routes[out->route_count];
        if (nb_cfg_copy(route->service, sizeof(route->service), parsed.service, NULL) != NB_OK) {
            rc = nb_cfg_fail(err, "routes.service", "service name too long");
        } else if (nb_cfg_copy(route->endpoint, sizeof(route->endpoint), parsed.endpoint, NULL) != NB_OK) {
            rc = nb_cfg_fail(err, "routes.endpoint", "endpoint too long");
        } else if (!route->service[0]) {
            rc = nb_cfg_fail(err, "routes.service", "missing service name");
        } else if (!route->endpoint[0]) {
            rc = nb_cfg_fail(err, "routes.endpoint", "missing endpoint");
        } else if (nb_endpoint_parse(route->endpoint, &endpoint) != NB_OK) {
            rc = nb_cfg_fail(err, "routes.endpoint", "invalid endpoint URI");
        }
        if (rc == NB_OK) {
            out->route_count++;
        }
        nb_codec_json_clear(&parsed, nb_g_route_model, CFG_MODEL_N(nb_g_route_model));
        if (rc != NB_OK) {
            return rc;
        }
    }
    return NB_OK;
}

/* 把中间结构合入 nb_config_t.node，应用默认值并校验范围。 */
static int nb_apply_config(nb_config_t *out,
                        const nb_cfg_runtime_in_t *rt,
                        const nb_cfg_transport_in_t *tp,
                        const nb_cfg_discovery_in_t *disc,
                        const nb_cfg_metrics_in_t *mt,
                        const nb_cfg_log_in_t *lg,
                        const cJSON *root,
                        const cJSON *bindings,
                        const cJSON *peers,
                        const cJSON *routes,
                        nb_config_error_t *err)
{
    nb_node_config_t *node = &out->node;
    nb_node_mode_t mode = NB_MODE_MESH;
    nb_transport_kind_t kind = NB_TRANSPORT_UDP;
    const char *name = NULL;
    const cJSON *item;
    int rc;

    nb_node_config_default(node);

    /* name / mode（顶层） */
    item = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (item) {
        if (!cJSON_IsString(item) || !item->valuestring) {
            return nb_cfg_fail(err, "name", "must be a string");
        }
        name = item->valuestring;
    }
    if (nb_cfg_copy(out->name, sizeof(out->name), name, NB_DEFAULT_NODE_NAME) != NB_OK) {
        return nb_cfg_fail(err, "name", "too long");
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "mode");
    if (item) {
        if (!cJSON_IsString(item) || nb_cfg_mode(item->valuestring, &mode) != NB_OK) {
            return nb_cfg_fail(err, "mode", "must be \"mesh\" or \"static\"");
        }
    }
    node->mode = mode;
    node->name = out->name;

    /* runtime */
    if (rt->io_threads > NB_MAX_IO_THREADS) {
        return nb_cfg_fail(err, "runtime.io_threads", "out of range");
    }
    if (rt->handler_threads > NB_MAX_HANDLER_THREADS) {
        return nb_cfg_fail(err, "runtime.handler_threads", "out of range");
    }
    node->runtime.io_threads = (unsigned)rt->io_threads;
    node->runtime.handler_threads = (unsigned)rt->handler_threads;
    node->runtime.handler_max_concurrency = (unsigned)rt->handler_max_concurrency;
    node->runtime.max_inflight = rt->max_inflight;
    node->runtime.max_streams = rt->max_streams;
    node->runtime.peer_rate_limit_pps = rt->peer_rate_limit_pps;

    /* transport */
    if (tp->kind && nb_cfg_transport_kind(tp->kind, &kind) != NB_OK) {
        return nb_cfg_fail(err, "transport.kind", "must be \"udp\" or \"tcp\"");
    }
    if (tp->data_port > UINT16_MAX) {
        return nb_cfg_fail(err, "transport.data_port", "out of range");
    }
    if (tp->frag_bytes > UINT16_MAX) {
        return nb_cfg_fail(err, "transport.frag_bytes", "out of range");
    }
    node->transport.transport = kind;
    if (nb_cfg_copy(out->bind_host, sizeof(out->bind_host), tp->bind_host, NB_DEFAULT_BIND_HOST) !=
        NB_OK) {
        return nb_cfg_fail(err, "transport.bind_host", "too long");
    }
    node->transport.bind_host = out->bind_host;
    node->transport.data_port = (uint16_t)tp->data_port;
    node->transport.frag_bytes = (uint16_t)tp->frag_bytes;
    node->transport.max_msg_bytes = tp->max_msg_bytes;
    node->transport.backpressure_bytes = tp->backpressure_bytes;
    node->transport.rtcp_interval_ms = tp->rtcp_interval_ms;
    node->transport.rtcp_disabled = tp->rtcp_present ? (tp->rtcp ? 0 : 1) : 0;

    /* discovery */
    if (disc->mode && nb_cfg_discovery_mode(disc->mode, &node->discovery.disabled) != NB_OK) {
        return nb_cfg_fail(err, "discovery.mode", "must be mesh/static/off/none");
    }
    if (disc->broadcast) {
        if (nb_cfg_host_port(
                disc->broadcast, out->discovery_host, sizeof(out->discovery_host), &node->discovery.port) !=
            NB_OK) {
            return nb_cfg_fail(err, "discovery.broadcast", "invalid host:port");
        }
    } else if (nb_cfg_copy(out->discovery_host,
                        sizeof(out->discovery_host),
                        NULL,
                        NB_DEFAULT_DISCOVERY_HOST) != NB_OK) {
        return nb_cfg_fail(err, "discovery.broadcast", "default host too long");
    }
    node->discovery.host = out->discovery_host;
    node->discovery.hello_interval_ms = disc->hello_interval_ms;
    node->discovery.node_ttl_ms = disc->node_ttl_ms;

    /* metrics */
    node->metrics.disabled = mt->disabled;
    node->metrics.latency_disabled = mt->latency_disabled;
    node->metrics.sample_every =
        mt->sample_every ? mt->sample_every : NB_DEFAULT_METRICS_SAMPLE_EVERY;
    node->metrics.per_peer_disabled = mt->per_peer_disabled;

    /* log */
    node->log.disabled = lg->disabled;
    if (lg->level && nb_cfg_log_level(lg->level, &node->log.level) != NB_OK) {
        return nb_cfg_fail(err, "log.level", "invalid level");
    }
    if (nb_cfg_copy(out->log_file, sizeof(out->log_file), lg->file, NULL) != NB_OK) {
        return nb_cfg_fail(err, "log.file", "too long");
    }
    node->log.file = lg->file ? out->log_file : NULL;

    rc = nb_fill_bindings(out, bindings, err);
    if (rc != NB_OK) {
        return rc;
    }
    rc = nb_fill_peers(out, peers, err);
    if (rc != NB_OK) {
        return rc;
    }
    rc = nb_fill_routes(out, routes, err);
    if (rc != NB_OK) {
        return rc;
    }
    if (out->binding_count > 0) {
        node->transport.bindings = out->binding_configs;
        node->transport.binding_count = out->binding_count;
    }
    nb_node_config_normalize(node);
    return NB_OK;
}

/* ---- 配置加载 ---- */

int nb_config_load_ex(const char *json, nb_config_t *out, nb_config_error_t *err)
{
    cJSON *root = NULL;
    nb_cfg_runtime_in_t rt;
    nb_cfg_transport_in_t tp;
    nb_cfg_discovery_in_t disc;
    nb_cfg_metrics_in_t mt;
    nb_cfg_log_in_t lg;
    const cJSON *bindings = NULL;
    const cJSON *peers = NULL;
    const cJSON *routes = NULL;
    int rc = NB_ERR_INVALID;

    if (err) {
        memset(err, 0, sizeof(*err));
    }
    if (!json || !out) {
        return nb_cfg_fail(err, "", "null argument");
    }
    memset(out, 0, sizeof(*out));
    memset(&rt, 0, sizeof(rt));
    memset(&tp, 0, sizeof(tp));
    memset(&disc, 0, sizeof(disc));
    memset(&mt, 0, sizeof(mt));
    memset(&lg, 0, sizeof(lg));

    root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        nb_cfg_fail(err, "", "malformed JSON object");
        goto done;
    }
    routes = cJSON_GetObjectItemCaseSensitive(root, "routes");
    if (routes && !cJSON_IsArray(routes)) {
        nb_cfg_fail(err, "routes", "must be an array");
        goto done;
    }
    rc = nb_load_runtime(root, &rt, err);
    if (rc != NB_OK) {
        goto done;
    }
    rc = nb_load_transport(root, &tp, &bindings, err);
    if (rc != NB_OK) {
        goto done;
    }
    rc = nb_load_discovery(root, &disc, &peers, err);
    if (rc != NB_OK) {
        goto done;
    }
    rc = nb_load_metrics(root, &mt, err);
    if (rc != NB_OK) {
        goto done;
    }
    rc = nb_load_log(root, &lg, err);
    if (rc != NB_OK) {
        goto done;
    }
    rc = nb_apply_config(out, &rt, &tp, &disc, &mt, &lg, root, bindings, peers, routes, err);
    if (rc != NB_OK) {
        memset(out, 0, sizeof(*out));
        goto done;
    }
    rc = NB_OK;

done:
    nb_codec_json_clear(&rt, nb_g_runtime_model, CFG_MODEL_N(nb_g_runtime_model));
    nb_codec_json_clear(&tp, nb_g_transport_model, CFG_MODEL_N(nb_g_transport_model));
    nb_codec_json_clear(&disc, nb_g_discovery_model, CFG_MODEL_N(nb_g_discovery_model));
    nb_codec_json_clear(&mt, nb_g_metrics_model, CFG_MODEL_N(nb_g_metrics_model));
    nb_codec_json_clear(&lg, nb_g_log_model, CFG_MODEL_N(nb_g_log_model));
    if (root) {
        cJSON_Delete(root);
    }
    return rc;
}

int nb_config_load(const char *json, nb_config_t *out)
{
    return nb_config_load_ex(json, out, NULL);
}

int nb_config_load_file_ex(const char *path, nb_config_t *out, nb_config_error_t *err)
{
    FILE *f;
    long size;
    char *buf;
    int rc;

    if (err) {
        memset(err, 0, sizeof(*err));
    }
    if (!path || !out) {
        return nb_cfg_fail(err, "", "null argument");
    }
    f = fopen(path, "rb");
    if (!f) {
        return nb_cfg_fail_code(err, NB_ERR_IO, "", "cannot open config file");
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return nb_cfg_fail_code(err, NB_ERR_IO, "", "cannot seek config file");
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return nb_cfg_fail_code(err, NB_ERR_IO, "", "cannot size config file");
    }
    rewind(f);
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return nb_cfg_fail_code(err, NB_ERR_NOMEM, "", "out of memory");
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return nb_cfg_fail_code(err, NB_ERR_IO, "", "cannot read config file");
    }
    buf[size] = '\0';
    fclose(f);
    rc = nb_config_load_ex(buf, out, err);
    free(buf);
    return rc;
}

int nb_config_load_file(const char *path, nb_config_t *out)
{
    return nb_config_load_file_ex(path, out, NULL);
}

/* ---- 节点创建 ---- */

int nb_node_create_from_config(const nb_config_t *cfg, nb_node_t **out)
{
    int rc;
    size_t i;
    if (!cfg || !out) {
        return NB_ERR_INVALID;
    }
    rc = nb_node_create(&cfg->node, out);
    if (rc != NB_OK) {
        return rc;
    }
    for (i = 0; i < cfg->peer_count; i++) {
        (void)nb_node_add_discovery_peer(*out, cfg->peers[i].ip, cfg->peers[i].port);
    }
    for (i = 0; i < cfg->route_count; i++) {
        (void)nb_node_add_static_endpoint(*out, cfg->routes[i].service, cfg->routes[i].endpoint);
    }
    return NB_OK;
}
