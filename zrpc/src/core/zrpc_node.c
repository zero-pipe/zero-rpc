/* 节点：生命周期、传输装配、线程/发现配置与阻塞运行（分发见 zrpc_dispatch.c）。 */
#include "zrpc_node_internal.h"

#include "zrpc_mem.h"
#include "zrpc_transport_factory.h"
#include "zrpc_util.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t zrpc_now_ms(void) {
    return ztk_monotonic_ms();
}

static uint32_t zrpc_rand(void) {
    static uint32_t st;
    if (st == 0) {
        st = (uint32_t)ztk_monotonic_ms() ^ 0x9e3779b9u;
    }
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

/* ---- 定时器 ---- */

static void hello_timer_cb(void *user) {
    zrpc_node_t *node = (zrpc_node_t *)user;
    if (node && node->registry_backend && node->registry_backend->ops->announce) {
        node->registry_backend->ops->announce(node, 0);
    }
}

static void node_timer_cb(void *user) {
    zrpc_node_t *node = (zrpc_node_t *)user;
    uint64_t now = zrpc_now_ms();
    if (node->registry_backend && node->registry_backend->ops->tick) {
        node->registry_backend->ops->tick(node, now);
    }
    zrpc_service_expire_streams(node, now);
    zrpc_pending_tick(node, now);
}

/* ---- 公共 API ---- */

int zrpc_node_create(const zrpc_node_config_t *cfg, zrpc_node_t **out) {
    zrpc_node_t *node;
    ztk_poller_pool_opts_t popts;
    static int platform_inited = 0;

    if (!cfg || !out) {
        return ZRPC_ERR_INVALID;
    }
    *out = NULL;
    node = (zrpc_node_t *)calloc(1, sizeof(*node));
    if (!node) {
        return ZRPC_ERR_NOMEM;
    }
    if (cfg->service_table && zrpc_install_service_table(cfg->service_table) != ZRPC_OK) {
        free(node);
        return ZRPC_ERR_INVALID;
    }
    node->cfg = *cfg;
    zrpc_copy_str(node->name, sizeof(node->name), cfg->name, "zrpc");
    zrpc_copy_str(node->bind_host, sizeof(node->bind_host), cfg->bind_host, "0.0.0.0");
    zrpc_copy_str(node->discovery_host, sizeof(node->discovery_host), cfg->discovery_host,
                  "255.255.255.255");
    node->discovery_port = cfg->discovery_port ? cfg->discovery_port : ZRPC_DISCOVERY_PORT;
    node->io_threads = cfg->io_threads ? cfg->io_threads : ztk_thread_hardware_concurrency();
    if (node->io_threads > ZRPC_IO_MAX) {
        node->io_threads = ZRPC_IO_MAX;
    }
    if (node->io_threads < 1) {
        node->io_threads = 1;
    }
    node->hello_interval_ms = cfg->hello_interval_ms ? cfg->hello_interval_ms : 3000;
    node->node_ttl_ms = cfg->node_ttl_ms ? cfg->node_ttl_ms : 9000;
    node->mode = cfg->mode;
    node->frag_bytes = cfg->frag_bytes;
    if (node->frag_bytes < ZRPC_FRAG_MIN) {
        node->frag_bytes = ZRPC_FRAG_DEFAULT;
    }
    if (node->frag_bytes > ZRPC_FRAG_MAX) {
        node->frag_bytes = ZRPC_FRAG_MAX;
    }
    node->max_msg_bytes = cfg->max_msg_bytes ? cfg->max_msg_bytes : ZRPC_MAX_MSG;
    if (node->max_msg_bytes > ZRPC_MAX_MSG_LIMIT) {
        node->max_msg_bytes = ZRPC_MAX_MSG_LIMIT;
    }
    if (node->max_msg_bytes < node->frag_bytes) {
        node->max_msg_bytes = node->frag_bytes;
    }
    node->bp_high_water = cfg->backpressure_bytes ? cfg->backpressure_bytes
                                                  : ZRPC_BACKPRESSURE_DEFAULT;
    node->rtcp_interval_ms = cfg->rtcp_interval_ms ? cfg->rtcp_interval_ms : ZRPC_RTCP_INTERVAL_MS;
    node->rtcp_disabled = cfg->rtcp_disabled;
    {
        const char *dp = getenv("ZRPC_DROP_PERCENT");
        int v = dp ? atoi(dp) : 0;
        if (v < 0) {
            v = 0;
        }
        if (v > 50) {
            v = 50;
        }
        node->drop_percent = (uint8_t)v;
    }
    node->node_id = zrpc_rand();
    if (node->node_id == 0) {
        node->node_id = 1;
    }
    node->next_request_id = 1;
    zrpc_route_init(&node->routes);
    zrpc_registry_init(&node->registry);
    node->registry_backend = (cfg->mode == ZRPC_MODE_MESH) ? zrpc_registry_backend_mesh() : NULL;
    node->id_lock = ztk_mutex_create(ZTK_MUTEX_NORMAL);
    if (!node->id_lock) {
        zrpc_node_destroy(node);
        return ZRPC_ERR_NOMEM;
    }

    if (!platform_inited) {
        ztk_platform_init();
        platform_inited = 1;
    }

    memset(&popts, 0, sizeof(popts));
    popts.size = node->io_threads;
    popts.thread_priority = ZTK_THREAD_PRIO_NORMAL;
    node->pool = ztk_poller_pool_create(&popts);
    if (!node->pool || ztk_poller_pool_start(node->pool) != ZTK_OK) {
        zrpc_node_destroy(node);
        return ZRPC_ERR_IO;
    }
    node->poller = ztk_poller_pool_at(node->pool, 0);

    /*
     * 挂载 per-poller 缓冲池，先把底层能力接上，热路径再逐步迁移到
     * ztk_buf_alloc_local / ztk_buf_pool_acquire。每档设上限，避免无界增长。
     */
    {
        ztk_buf_pool_opts bopts;
        memset(&bopts, 0, sizeof(bopts));
        bopts.max_per_bucket = ZRPC_BUF_POOL_MAX_PER_BUCKET;
        bopts.thread_safe = 0; /* poller 本地，热路径无锁 */
        if (ztk_poller_pool_attach_buf_pools(node->pool, &bopts) != ZTK_OK) {
            zrpc_node_destroy(node);
            return ZRPC_ERR_NOMEM;
        }
    }

    if (cfg->bindings && cfg->binding_count > 0) {
        size_t i;
        for (i = 0; i < cfg->binding_count; i++) {
            zrpc_endpoint_t endpoint;
            const zrpc_binding_config_t *binding = &cfg->bindings[i];
            if (!binding->endpoint || zrpc_endpoint_parse(binding->endpoint, &endpoint) != ZRPC_OK) {
                zrpc_node_destroy(node);
                return ZRPC_ERR_INVALID;
            }
            if (zrpc_node_transport_add(node, endpoint.scheme, endpoint.host, endpoint.port) !=
                ZRPC_OK) {
                zrpc_node_destroy(node);
                return ZRPC_ERR_IO;
            }
        }
    } else {
        const char *scheme = cfg->transport == ZRPC_TRANSPORT_TCP ? "tcp" : "udp";
        if (zrpc_node_transport_add(node, scheme, node->bind_host, cfg->data_port) != ZRPC_OK) {
            zrpc_node_destroy(node);
            return ZRPC_ERR_IO;
        }
    }

    if (node->mode == ZRPC_MODE_MESH) {
        if (!node->registry_backend || !node->registry_backend->ops->start ||
            node->registry_backend->ops->start(node) != ZRPC_OK) {
            zrpc_node_destroy(node);
            return ZRPC_ERR_IO;
        }
        node->hello_timer =
            ztk_timer_start(node->poller, node->hello_interval_ms, 1, hello_timer_cb, node);
    }
    node->node_timer =
        ztk_timer_start(node->poller, ZRPC_TICK_INTERVAL_MS, 1, node_timer_cb, node);
    node->started = 1;

    if (node->mode == ZRPC_MODE_MESH) {
        if (node->registry_backend && node->registry_backend->ops->announce) {
            node->registry_backend->ops->announce(node, 0);
        }
    }
    *out = node;
    return ZRPC_OK;
}

void zrpc_node_destroy(zrpc_node_t *node) {
    if (!node) {
        return;
    }
    if (node->started && node->mode == ZRPC_MODE_MESH) {
        if (node->registry_backend && node->registry_backend->ops->announce) {
            node->registry_backend->ops->announce(node, 1); /* BYE */
        }
    }
    /* Quiesce poller callbacks before destroying sockets or transport state. */
    if (node->pool) {
        ztk_poller_pool_stop(node->pool);
    }
    if (node->hello_timer) {
        ztk_timer_stop(node->hello_timer);
        node->hello_timer = NULL;
    }
    if (node->node_timer) {
        ztk_timer_stop(node->node_timer);
        node->node_timer = NULL;
    }
    {
        size_t i;
        for (i = 0; i < node->transport_count; i++) {
            if (node->transports[i]) {
                zrpc_transport_stop(node->transports[i]);
            }
        }
    }
    if (node->registry_backend && node->registry_backend->ops->stop) {
        node->registry_backend->ops->stop(node);
    }
    {
        int i;
        for (i = 0; i < ZRPC_PENDING_MAX; i++) {
            zrpc_pending_release(&node->pending[i]);
        }
    }
    zrpc_route_cleanup(&node->routes);
    zrpc_registry_cleanup(&node->registry);
    {
        size_t i;
        for (i = 0; i < node->transport_count; i++) {
            if (node->transports[i]) {
                zrpc_transport_destroy(node->transports[i]);
                node->transports[i] = NULL;
            }
        }
        node->transport = NULL;
        node->transport_count = 0;
    }
    if (node->pool) {
        ztk_poller_pool_destroy(node->pool);
        node->pool = NULL;
    }
    {
        zrpc_service_t *svc = node->services;
        while (svc) {
            zrpc_service_t *next = svc->next;
            zrpc_method_t *m = svc->methods;
            while (m) {
                zrpc_method_t *mn = m->next;
                if (m->owns_user) {
                    free(m->user);
                }
                free(m);
                m = mn;
            }
            free(svc);
            svc = next;
        }
        node->services = NULL;
    }
    {
        zrpc_codec_entry_t *c = node->codecs;
        while (c) {
            zrpc_codec_entry_t *next = c->next;
            free(c);
            c = next;
        }
        node->codecs = NULL;
    }
    if (node->id_lock) {
        ztk_mutex_destroy(node->id_lock);
    }
    {
        int i;
        for (i = 0; i < ZRPC_MAX_RECV_STREAMS; i++) {
            if (node->recv_streams[i].used && node->recv_streams[i].call) {
                zrpc_mem_free(node->recv_streams[i].call);
                node->recv_streams[i].call = NULL;
                node->recv_streams[i].used = 0;
            }
        }
    }
    free(node);
}

const char *zrpc_node_name(const zrpc_node_t *node) {
    return node ? node->name : NULL;
}

zrpc_transport_kind_t zrpc_node_transport(const zrpc_node_t *node) {
    return (node && node->transport) ? node->transport->kind : ZRPC_TRANSPORT_UDP;
}

uint16_t zrpc_node_data_port(const zrpc_node_t *node) {
    return (node && node->transport) ? node->transport->port : 0;
}

size_t zrpc_node_binding_count(const zrpc_node_t *node) {
    return node ? node->transport_count : 0;
}

int zrpc_node_binding_endpoint(const zrpc_node_t *node, size_t index, char *out, size_t cap) {
    zrpc_endpoint_t endpoint;
    if (!node || index >= node->transport_count || !node->transports[index]) {
        return ZRPC_ERR_NOTFOUND;
    }
    memset(&endpoint, 0, sizeof(endpoint));
    zrpc_copy_str(endpoint.scheme, sizeof(endpoint.scheme), node->transports[index]->scheme, "udp");
    zrpc_copy_str(endpoint.host, sizeof(endpoint.host), node->transports[index]->bind_host, "0.0.0.0");
    endpoint.port = node->transports[index]->port;
    endpoint.transport = node->transports[index]->kind;
    return zrpc_endpoint_format(&endpoint, out, cap);
}

uint16_t zrpc_node_discovery_port(const zrpc_node_t *node) {
    return node ? node->discovery_port : 0;
}

int zrpc_node_add_discovery_peer(zrpc_node_t *node, const char *ip, uint16_t discovery_port) {
    int i;
    if (!node || !ip || !discovery_port) {
        return ZRPC_ERR_INVALID;
    }
    for (i = 0; i < ZRPC_DISC_TARGET_MAX; i++) {
        if (!node->discovery.targets[i].used) {
            node->discovery.targets[i].used = 1;
            zrpc_copy_str(node->discovery.targets[i].ip, sizeof(node->discovery.targets[i].ip), ip,
                          "127.0.0.1");
            node->discovery.targets[i].port = discovery_port;
            if (node->registry_backend && node->registry_backend->ops->announce) {
                node->registry_backend->ops->announce(node, 0);
            }
            return ZRPC_OK;
        }
    }
    return ZRPC_ERR_NOMEM;
}

int zrpc_node_add_static_route(zrpc_node_t *node, const char *service, const char *ip,
                               uint16_t port, zrpc_transport_kind_t transport) {
    return zrpc_route_set(&node->routes, service, ip, port, transport);
}

int zrpc_node_add_static_endpoint(zrpc_node_t *node, const char *service, const char *endpoint) {
    return zrpc_route_add_endpoint(&node->routes, service, endpoint, 1);
}

void zrpc_node_metrics(const zrpc_node_t *node, zrpc_metrics_t *out) {
    if (!out) {
        return;
    }
    if (!node) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = node->metrics;
}

int zrpc_node_rtcp_peer(const zrpc_node_t *node, const char *ip, uint16_t port,
                        zrpc_rtcp_peer_stats_t *out) {
    size_t i;
    if (!node || !ip || !out) {
        return ZRPC_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    for (i = 0; i < node->transport_count; i++) {
        zrpc_transport_t *t = node->transports[i];
        if (t && t->ops && t->ops->rtcp_peer && t->ops->rtcp_peer(t, ip, port, out) == 1) {
            return ZRPC_OK;
        }
    }
    memset(out, 0, sizeof(*out));
    return ZRPC_ERR_NOTFOUND;
}

/* ---- 在 poller 线程投递任务 ---- */

typedef struct zrpc_post_task {
    void (*fn)(void *user);
    void *user;
} zrpc_post_task_t;

static void post_trampoline(void *user) {
    zrpc_post_task_t *t = (zrpc_post_task_t *)user;
    t->fn(t->user);
    zrpc_mem_free(t);
}

int zrpc_node_post(zrpc_node_t *node, void (*fn)(void *user), void *user) {
    zrpc_post_task_t *t;
    if (!node || !fn) {
        return ZRPC_ERR_INVALID;
    }
    t = (zrpc_post_task_t *)zrpc_mem_alloc(sizeof(*t));
    if (!t) {
        return ZRPC_ERR_NOMEM;
    }
    memset(t, 0, sizeof(*t));
    t->fn = fn;
    t->user = user;
    if (ztk_poller_async(node->poller, post_trampoline, t, 1) != ZTK_OK) {
        zrpc_mem_free(t);
        return ZRPC_ERR_IO;
    }
    return ZRPC_OK;
}

static uint64_t post_delay_cb(void *user) {
    zrpc_post_task_t *t = (zrpc_post_task_t *)user;
    t->fn(t->user);
    zrpc_mem_free(t);
    return 0; /* 单次 */
}

int zrpc_node_post_delay(zrpc_node_t *node, uint64_t delay_ms, void (*fn)(void *user), void *user) {
    zrpc_post_task_t *t;
    if (!node || !fn) {
        return ZRPC_ERR_INVALID;
    }
    t = (zrpc_post_task_t *)zrpc_mem_alloc(sizeof(*t));
    if (!t) {
        return ZRPC_ERR_NOMEM;
    }
    memset(t, 0, sizeof(*t));
    t->fn = fn;
    t->user = user;
    if (ztk_poller_do_delay(node->poller, delay_ms, post_delay_cb, t) == NULL) {
        zrpc_mem_free(t);
        return ZRPC_ERR_IO;
    }
    return ZRPC_OK;
}

int zrpc_node_wait_for_service(zrpc_node_t *node, const char *service, uint64_t timeout_ms) {
    uint64_t deadline;
    char ip[ZRPC_ENDPOINT_MAX];
    uint16_t port = 0;
    if (!node || !service) {
        return ZRPC_ERR_INVALID;
    }
    if (node->mode == ZRPC_MODE_STATIC) {
        return ZRPC_OK;
    }
    deadline = zrpc_now_ms() + (timeout_ms ? timeout_ms : ZRPC_DEFAULT_TIMEOUT_MS);
    for (;;) {
        if (zrpc_route_lookup(&node->routes, service, ip, sizeof(ip), &port, NULL, NULL, 0) ==
            ZRPC_OK) {
            return ZRPC_OK;
        }
        {
            zrpc_endpoint_t endpoint;
            if (zrpc_registry_select(&node->registry, service, &endpoint) == ZRPC_OK) {
                return ZRPC_OK;
            }
        }
        if (zrpc_now_ms() >= deadline) {
            return ZRPC_ERR_TIMEOUT;
        }
        ztk_sleep_ms(10);
    }
}

/* ---- 阻塞运行 ---- */

static volatile sig_atomic_t g_zrpc_signal;

static void zrpc_signal_handler(int signo) {
    (void)signo;
    g_zrpc_signal = 1;
}

int zrpc_node_spin_once(zrpc_node_t *node, uint64_t timeout_ms) {
    uint64_t deadline;
    int installed = 0;
    if (!node) {
        return ZRPC_ERR_INVALID;
    }
    if (!g_zrpc_signal && signal(SIGINT, zrpc_signal_handler) != SIG_ERR) {
        (void)signal(SIGTERM, zrpc_signal_handler);
        installed = 1;
    }
    (void)installed;
    deadline = zrpc_now_ms() + timeout_ms;
    while (!g_zrpc_signal) {
        if (timeout_ms && zrpc_now_ms() >= deadline) {
            return ZRPC_ERR_TIMEOUT;
        }
        ztk_sleep_ms(20);
    }
    return ZRPC_OK;
}

int zrpc_node_spin(zrpc_node_t *node) {
    return zrpc_node_spin_once(node, 0);
}
