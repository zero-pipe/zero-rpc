/* 节点：生命周期、传输装配、线程/发现配置与阻塞运行（分发见 zri_dispatch.c）。 */
#include "node_internal.h"

#include "mem.h"
#include "transport_factory.h"
#include "util.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t zri_now_ms(void) {
    return ntk_monotonic_ms();
}

static uint32_t zri_rand(void) {
    static uint32_t st;
    if (st == 0) {
        st = (uint32_t)ntk_monotonic_ms() ^ 0x9e3779b9u;
    }
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

/* ---- 定时器 ---- */

static void hello_timer_cb(void *user) {
    node_t *node = (node_t *)user;
    if (node && node->registry_backend && node->registry_backend->ops->announce) {
        node->registry_backend->ops->announce(node, 0);
    }
}

static void node_timer_cb(void *user) {
    node_t *node = (node_t *)user;
    uint64_t now = zri_now_ms();
    if (node->registry_backend && node->registry_backend->ops->tick) {
        node->registry_backend->ops->tick(node, now);
    }
    zri_service_expire_streams(node, now);
    zri_pending_tick(node, now);
}

/* ---- 公共 API ---- */

int node_create(const node_config_t *cfg, node_t **out) {
    node_t *node;
    ntk_poller_pool_opts_t popts;
    static int platform_inited = 0;

    if (!cfg || !out) {
        return ERR_INVALID;
    }
    *out = NULL;
    node = (node_t *)calloc(1, sizeof(*node));
    if (!node) {
        return ERR_NOMEM;
    }
    if (cfg->service_table && install_service_table(cfg->service_table) != OK) {
        free(node);
        return ERR_INVALID;
    }
    node->cfg = *cfg;
    zri_copy_str(node->name, sizeof(node->name), cfg->name, "rpc");
    zri_copy_str(node->bind_host, sizeof(node->bind_host), cfg->bind_host, "0.0.0.0");
    zri_copy_str(node->discovery_host, sizeof(node->discovery_host), cfg->discovery_host,
                  "255.255.255.255");
    node->discovery_port = cfg->discovery_port ? cfg->discovery_port : DISCOVERY_PORT;
    node->io_threads = cfg->io_threads ? cfg->io_threads : ntk_thread_hardware_concurrency();
    if (node->io_threads > IO_MAX) {
        node->io_threads = IO_MAX;
    }
    if (node->io_threads < 1) {
        node->io_threads = 1;
    }
    node->hello_interval_ms = cfg->hello_interval_ms ? cfg->hello_interval_ms : 3000;
    node->node_ttl_ms = cfg->node_ttl_ms ? cfg->node_ttl_ms : 9000;
    node->mode = cfg->mode;
    node->frag_bytes = cfg->frag_bytes;
    if (node->frag_bytes < FRAG_MIN) {
        node->frag_bytes = FRAG_DEFAULT;
    }
    if (node->frag_bytes > FRAG_MAX) {
        node->frag_bytes = FRAG_MAX;
    }
    node->max_msg_bytes = cfg->max_msg_bytes ? cfg->max_msg_bytes : MAX_MSG;
    if (node->max_msg_bytes > MAX_MSG_LIMIT) {
        node->max_msg_bytes = MAX_MSG_LIMIT;
    }
    if (node->max_msg_bytes < node->frag_bytes) {
        node->max_msg_bytes = node->frag_bytes;
    }
    node->bp_high_water = cfg->backpressure_bytes ? cfg->backpressure_bytes
                                                  : BACKPRESSURE_DEFAULT;
    node->rtcp_interval_ms = cfg->rtcp_interval_ms ? cfg->rtcp_interval_ms : RTCP_INTERVAL_MS;
    node->rtcp_disabled = cfg->rtcp_disabled;
    {
        const char *dp = getenv("DROP_PERCENT");
        int v = dp ? atoi(dp) : 0;
        if (v < 0) {
            v = 0;
        }
        if (v > 50) {
            v = 50;
        }
        node->drop_percent = (uint8_t)v;
    }
    node->node_id = zri_rand();
    if (node->node_id == 0) {
        node->node_id = 1;
    }
    node->next_request_id = 1;
    zri_route_init(&node->routes);
    zri_registry_init(&node->registry);
    node->registry_backend = (cfg->mode == MODE_MESH) ? zri_registry_backend_mesh() : NULL;
    node->id_lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    if (!node->id_lock) {
        node_destroy(node);
        return ERR_NOMEM;
    }

    if (!platform_inited) {
        ntk_platform_init();
        platform_inited = 1;
    }

    memset(&popts, 0, sizeof(popts));
    popts.size = node->io_threads;
    popts.thread_priority = NTK_THREAD_PRIO_NORMAL;
    node->pool = ntk_poller_pool_create(&popts);
    if (!node->pool || ntk_poller_pool_start(node->pool) != NTK_OK) {
        node_destroy(node);
        return ERR_IO;
    }
    node->poller = ntk_poller_pool_at(node->pool, 0);

    /*
     * 挂载 per-poller 缓冲池，先把底层能力接上，热路径再逐步迁移到
     * ntk_buf_alloc_local / ntk_buf_pool_acquire。每档设上限，避免无界增长。
     */
    {
        ntk_buf_pool_opts bopts;
        memset(&bopts, 0, sizeof(bopts));
        bopts.max_per_bucket = BUF_POOL_MAX_PER_BUCKET;
        bopts.thread_safe = 0; /* poller 本地，热路径无锁 */
        if (ntk_poller_pool_attach_buf_pools(node->pool, &bopts) != NTK_OK) {
            node_destroy(node);
            return ERR_NOMEM;
        }
    }

    if (cfg->bindings && cfg->binding_count > 0) {
        size_t i;
        for (i = 0; i < cfg->binding_count; i++) {
            endpoint_t endpoint;
            const binding_config_t *binding = &cfg->bindings[i];
            if (!binding->endpoint || endpoint_parse(binding->endpoint, &endpoint) != OK) {
                node_destroy(node);
                return ERR_INVALID;
            }
            if (zri_node_transport_add(node, endpoint.scheme, endpoint.host, endpoint.port) !=
                OK) {
                node_destroy(node);
                return ERR_IO;
            }
        }
    } else {
        const char *scheme = cfg->transport == TRANSPORT_TCP ? "tcp" : "udp";
        if (zri_node_transport_add(node, scheme, node->bind_host, cfg->data_port) != OK) {
            node_destroy(node);
            return ERR_IO;
        }
    }

    if (node->mode == MODE_MESH) {
        if (!node->registry_backend || !node->registry_backend->ops->start ||
            node->registry_backend->ops->start(node) != OK) {
            node_destroy(node);
            return ERR_IO;
        }
        node->hello_timer =
            ntk_timer_start(node->poller, node->hello_interval_ms, 1, hello_timer_cb, node);
    }
    node->node_timer =
        ntk_timer_start(node->poller, zri_tick_interval_ms(), 1, node_timer_cb, node);
    node->started = 1;

    if (node->mode == MODE_MESH) {
        if (node->registry_backend && node->registry_backend->ops->announce) {
            node->registry_backend->ops->announce(node, 0);
        }
    }
    *out = node;
    return OK;
}

void node_destroy(node_t *node) {
    if (!node) {
        return;
    }
    if (node->started && node->mode == MODE_MESH) {
        if (node->registry_backend && node->registry_backend->ops->announce) {
            node->registry_backend->ops->announce(node, 1); /* BYE */
        }
    }
    /* Quiesce poller callbacks before destroying sockets or transport state. */
    if (node->pool) {
        ntk_poller_pool_stop(node->pool);
    }
    if (node->hello_timer) {
        ntk_timer_stop(node->hello_timer);
        node->hello_timer = NULL;
    }
    if (node->node_timer) {
        ntk_timer_stop(node->node_timer);
        node->node_timer = NULL;
    }
    {
        size_t i;
        for (i = 0; i < node->transport_count; i++) {
            if (node->transports[i]) {
                zri_transport_stop(node->transports[i]);
            }
        }
    }
    if (node->registry_backend && node->registry_backend->ops->stop) {
        node->registry_backend->ops->stop(node);
    }
    {
        int i;
        for (i = 0; i < PENDING_MAX; i++) {
            zri_pending_release(&node->pending[i]);
        }
    }
    zri_route_cleanup(&node->routes);
    zri_registry_cleanup(&node->registry);
    {
        size_t i;
        for (i = 0; i < node->transport_count; i++) {
            if (node->transports[i]) {
                zri_transport_destroy(node->transports[i]);
                node->transports[i] = NULL;
            }
        }
        node->transport = NULL;
        node->transport_count = 0;
    }
    if (node->pool) {
        ntk_poller_pool_destroy(node->pool);
        node->pool = NULL;
    }
    {
        service_t *svc = node->services;
        while (svc) {
            service_t *next = svc->next;
            zri_method_t *m = svc->methods;
            while (m) {
                zri_method_t *mn = m->next;
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
        zri_codec_entry_t *c = node->codecs;
        while (c) {
            zri_codec_entry_t *next = c->next;
            free(c);
            c = next;
        }
        node->codecs = NULL;
    }
    if (node->id_lock) {
        ntk_mutex_destroy(node->id_lock);
    }
    {
        int i;
        for (i = 0; i < MAX_RECV_STREAMS; i++) {
            if (node->recv_streams[i].used && node->recv_streams[i].call) {
                zri_mem_free(node->recv_streams[i].call);
                node->recv_streams[i].call = NULL;
                node->recv_streams[i].used = 0;
            }
        }
    }
    free(node);
}

/* ---- 节点状态与端点 ---- */

const char *node_name(const node_t *node) {
    return node ? node->name : NULL;
}

transport_kind_t node_transport(const node_t *node) {
    return (node && node->transport) ? node->transport->kind : TRANSPORT_UDP;
}

uint16_t node_discovery_port(const node_t *node) {
    return node ? node->discovery_port : 0;
}

uint16_t node_data_port(const node_t *node) {
    return (node && node->transport) ? node->transport->port : 0;
}

size_t node_binding_count(const node_t *node) {
    return node ? node->transport_count : 0;
}

int node_binding_endpoint(const node_t *node, size_t index, char *out, size_t cap) {
    endpoint_t endpoint;
    if (!node || index >= node->transport_count || !node->transports[index]) {
        return ERR_NOTFOUND;
    }
    memset(&endpoint, 0, sizeof(endpoint));
    zri_copy_str(endpoint.scheme, sizeof(endpoint.scheme), node->transports[index]->scheme, "udp");
    zri_copy_str(endpoint.host, sizeof(endpoint.host), node->transports[index]->bind_host, "0.0.0.0");
    endpoint.port = node->transports[index]->port;
    endpoint.transport = node->transports[index]->kind;
    return endpoint_format(&endpoint, out, cap);
}

/* ---- 指标与对端质量 ---- */

void node_metrics(const node_t *node, metrics_t *out) {
    if (!out) {
        return;
    }
    if (!node) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = node->metrics;
}

int node_peer_stats(const node_t *node, const char *ip, uint16_t port,
                         peer_stats_t *out) {
    size_t i;
    if (!node || !ip || !out) {
        return ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    for (i = 0; i < node->transport_count; i++) {
        zri_transport_t *t = node->transports[i];
        if (t && t->ops && t->ops->peer_stats && t->ops->peer_stats(t, ip, port, out) == 1) {
            return OK;
        }
    }
    memset(out, 0, sizeof(*out));
    return ERR_NOTFOUND;
}

int node_rtcp_peer(const node_t *node, const char *ip, uint16_t port,
                        rtcp_peer_stats_t *out) {
    peer_stats_t stats;
    int rc;

    if (!out) {
        return ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    rc = node_peer_stats(node, ip, port, &stats);
    if (rc != OK) {
        return rc;
    }
    out->ssrc_local = stats.local_id;
    out->ssrc_remote = stats.remote_id;
    out->rtt_ms = stats.rtt_ms;
    out->jitter_ms = stats.jitter_ms;
    out->remote_lost = stats.lost;
    /* 旧接口无法区分 SR/RR，只能把统一反馈计数映射到 SR。 */
    out->sr_sent = stats.feedback_sent;
    out->sr_received = stats.feedback_received;
    return OK;
}

/* ---- 发现与路由 ---- */

int node_add_discovery_peer(node_t *node, const char *ip, uint16_t discovery_port) {
    int i;
    if (!node || !ip || !discovery_port) {
        return ERR_INVALID;
    }
    for (i = 0; i < DISC_TARGET_MAX; i++) {
        if (!node->discovery.targets[i].used) {
            node->discovery.targets[i].used = 1;
            zri_copy_str(node->discovery.targets[i].ip, sizeof(node->discovery.targets[i].ip), ip,
                          "127.0.0.1");
            node->discovery.targets[i].port = discovery_port;
            if (node->registry_backend && node->registry_backend->ops->announce) {
                node->registry_backend->ops->announce(node, 0);
            }
            return OK;
        }
    }
    return ERR_NOMEM;
}

int node_add_static_route(node_t *node, const char *service, const char *ip,
                               uint16_t port, transport_kind_t transport) {
    return zri_route_set(&node->routes, service, ip, port, transport);
}

int node_add_static_endpoint(node_t *node, const char *service, const char *endpoint) {
    return zri_route_add_endpoint(&node->routes, service, endpoint, 1);
}

/* ---- 服务等待与调度 ---- */

int node_wait_for_service(node_t *node, const char *service, uint64_t timeout_ms) {
    uint64_t deadline;
    char ip[ENDPOINT_MAX];
    uint16_t port = 0;
    if (!node || !service) {
        return ERR_INVALID;
    }
    if (node->mode == MODE_STATIC) {
        return OK;
    }
    deadline = zri_now_ms() + (timeout_ms ? timeout_ms : DEFAULT_TIMEOUT_MS);
    for (;;) {
        if (zri_route_lookup(&node->routes, service, ip, sizeof(ip), &port, NULL, NULL, 0) ==
            OK) {
            return OK;
        }
        {
            endpoint_t endpoint;
            if (zri_registry_select(&node->registry, service, &endpoint) == OK) {
                return OK;
            }
        }
        if (zri_now_ms() >= deadline) {
            return ERR_TIMEOUT;
        }
        ntk_sleep_ms(10);
    }
}

/* ---- 在 poller 线程投递任务 ---- */

typedef struct zri_post_task {
    void (*fn)(void *user);
    void *user;
} zri_post_task_t;

static void post_trampoline(void *user) {
    zri_post_task_t *t = (zri_post_task_t *)user;
    t->fn(t->user);
    zri_mem_free(t);
}

int node_post(node_t *node, void (*fn)(void *user), void *user) {
    zri_post_task_t *t;
    if (!node || !fn) {
        return ERR_INVALID;
    }
    t = (zri_post_task_t *)zri_mem_alloc(sizeof(*t));
    if (!t) {
        return ERR_NOMEM;
    }
    memset(t, 0, sizeof(*t));
    t->fn = fn;
    t->user = user;
    if (ntk_poller_async(node->poller, post_trampoline, t, 1) != NTK_OK) {
        zri_mem_free(t);
        return ERR_IO;
    }
    return OK;
}

static uint64_t post_delay_cb(void *user) {
    zri_post_task_t *t = (zri_post_task_t *)user;
    t->fn(t->user);
    zri_mem_free(t);
    return 0; /* 单次 */
}

int node_post_delay(node_t *node, uint64_t delay_ms, void (*fn)(void *user), void *user) {
    zri_post_task_t *t;
    if (!node || !fn) {
        return ERR_INVALID;
    }
    t = (zri_post_task_t *)zri_mem_alloc(sizeof(*t));
    if (!t) {
        return ERR_NOMEM;
    }
    memset(t, 0, sizeof(*t));
    t->fn = fn;
    t->user = user;
    if (ntk_poller_do_delay(node->poller, delay_ms, post_delay_cb, t) == NULL) {
        zri_mem_free(t);
        return ERR_IO;
    }
    return OK;
}

/* ---- 阻塞运行 ---- */

static volatile sig_atomic_t g_signal;

static void zri_signal_handler(int signo) {
    (void)signo;
    g_signal = 1;
}

int node_spin_once(node_t *node, uint64_t timeout_ms) {
    uint64_t deadline;
    int installed = 0;
    if (!node) {
        return ERR_INVALID;
    }
    if (!g_signal && signal(SIGINT, zri_signal_handler) != SIG_ERR) {
        (void)signal(SIGTERM, zri_signal_handler);
        installed = 1;
    }
    (void)installed;
    deadline = zri_now_ms() + timeout_ms;
    while (!g_signal) {
        if (timeout_ms && zri_now_ms() >= deadline) {
            return ERR_TIMEOUT;
        }
        ntk_sleep_ms(20);
    }
    return OK;
}

int node_spin(node_t *node) {
    return node_spin_once(node, 0);
}
