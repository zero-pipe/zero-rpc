/* 节点：生命周期、传输装配、线程/发现配置与阻塞运行（分发见 zri_dispatch.c）。 */
#include "node_internal.h"

#include "atomic.h"
#include "mem.h"
#include "transport_factory.h"
#include "util.h"

#include <log.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t zri_now_ms(void)
{
    return ntk_monotonic_ms();
}

/* 出站分片：按 key 稳定选择 poller；未启用池时回退到 node->poller。 */
ntk_poller *zri_node_pick_poller(const node_t *node, uint64_t key)
{
    ntk_poller *poller = NULL;
    if (!node) {
        return NULL;
    }
    if (node->pool) {
        poller = ntk_poller_pool_get_by_key((ntk_poller_pool *)node->pool, key);
    }
    return poller ? poller : node->poller;
}

/* poller 在 node 出站分片中的下标；找不到返回 -1。 */
int zri_node_poller_index(const node_t *node, ntk_poller *poller)
{
    unsigned i;
    if (!node || !poller) {
        return -1;
    }
    for (i = 0; i < node->io_count; i++) {
        if (node->io_pollers[i] == poller) {
            return (int)i;
        }
    }
    if (node->poller == poller) {
        return 0;
    }
    return -1;
}

static uint32_t zri_rand(void)
{
    static uint32_t st;
    if (st == 0) {
        st = (uint32_t)ntk_monotonic_ms() ^ 0x9e3779b9u;
    }
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

/* ---- 定长对象复用池 ---- */

int zri_obj_pool_init(zri_obj_pool_t *pool, unsigned limit)
{
    if (!pool) {
        return ERR_INVALID;
    }
    memset(pool, 0, sizeof(*pool));
    pool->limit = limit ? limit : 256;
    pool->lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    return pool->lock ? OK : ERR_NOMEM;
}

void zri_obj_pool_destroy(zri_obj_pool_t *pool)
{
    void *obj;
    if (!pool) {
        return;
    }
    obj = pool->free_list;
    while (obj) {
        void *next = *(void **)obj;
        free(obj);
        obj = next;
    }
    pool->free_list = NULL;
    pool->count = 0;
    if (pool->lock) {
        ntk_mutex_destroy(pool->lock);
        pool->lock = NULL;
    }
}

void *zri_obj_pool_alloc(zri_obj_pool_t *pool, size_t size)
{
    void *obj = NULL;
    if (!pool || !pool->lock) {
        return malloc(size);
    }
    if (ntk_mutex_trylock(pool->lock) == NTK_OK) {
        if (pool->free_list) {
            obj = pool->free_list;
            pool->free_list = *(void **)obj;
            if (pool->count > 0) {
                pool->count--;
            }
        }
        ntk_mutex_unlock(pool->lock);
    }
    return obj ? obj : malloc(size);
}

void zri_obj_pool_free(zri_obj_pool_t *pool, void *obj)
{
    if (!obj) {
        return;
    }
    if (pool && pool->lock && ntk_mutex_trylock(pool->lock) == NTK_OK) {
        if (pool->count < pool->limit) {
            *(void **)obj = pool->free_list;
            pool->free_list = obj;
            pool->count++;
            obj = NULL;
        }
        ntk_mutex_unlock(pool->lock);
    }
    if (obj) {
        free(obj);
    }
}

/* ---- 定时器 ---- */

static void hello_timer_cb(void *user)
{
    node_t *node = (node_t *)user;
    if (node && node->registry_backend && node->registry_backend->ops->announce) {
        node->registry_backend->ops->announce(node, 0);
    }
}

static void node_timer_cb(void *user)
{
    node_t *node = (node_t *)user;
    uint64_t now = zri_now_ms();
    if (node->registry_backend && node->registry_backend->ops->tick) {
        node->registry_backend->ops->tick(node, now);
    }
    zri_service_expire_streams(node, now);
    zri_node_expire_cancels(node, now);
    zri_dedup_expire(&node->dedup, now);
}

/* 每个 poller 各自驱动本 lane 上 pending 的超时/重传。 */
static void pending_timer_cb(void *user)
{
    node_t *node = (node_t *)user;
    zri_pending_tick(node, zri_now_ms(), ntk_poller_current());
}

/* ---- 公共 API ---- */

int node_create(const node_config_t *cfg, node_t **out)
{
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
    zri_copy_str(node->name, sizeof(node->name), cfg->name, "rpc");
    zri_copy_str(node->bind_host, sizeof(node->bind_host), cfg->bind_host, "0.0.0.0");
    zri_copy_str(
        node->discovery_host, sizeof(node->discovery_host), cfg->discovery_host, "255.255.255.255");
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
    node->bp_high_water = cfg->backpressure_bytes ? cfg->backpressure_bytes : BACKPRESSURE_DEFAULT;
    node->rtcp_interval_ms = cfg->rtcp_interval_ms ? cfg->rtcp_interval_ms : RTCP_INTERVAL_MS;
    node->rtcp_disabled = cfg->rtcp_disabled;
    node->max_recv_streams = cfg->max_streams ? cfg->max_streams : MAX_RECV_STREAMS;
    if (node->max_recv_streams > MAX_RECV_STREAMS) {
        node->max_recv_streams = MAX_RECV_STREAMS;
    }
    node->rate_limit_pps = cfg->peer_rate_limit_pps;
    node->handler_max_concurrency = cfg->handler_max_concurrency;
    node->max_inflight = cfg->max_inflight ? cfg->max_inflight : DEFAULT_MAX_INFLIGHT;
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
    node->services_lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    node->codecs_lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    node->pending_lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    node->rate_lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    if (!node->services_lock || !node->codecs_lock || !node->pending_lock || !node->rate_lock) {
        node_destroy(node);
        return ERR_NOMEM;
    }
    /* 保护 worker 限流计数与自定义 executor 诊断计数。 */
    node->handler_limit_mutex = ntk_mutex_create(NTK_MUTEX_NORMAL);
    if (!node->handler_limit_mutex) {
        node_destroy(node);
        return ERR_NOMEM;
    }
    if (zri_obj_pool_init(&node->worker_call_pool, 256) != OK) {
        node_destroy(node);
        return ERR_NOMEM;
    }
    if (zri_obj_pool_init(&node->handler_task_pool, 256) != OK ||
        zri_obj_pool_init(&node->reply_task_pool, 256) != OK) {
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
    {
        unsigned i;
        unsigned n = ntk_poller_pool_size(node->pool);
        if (n > IO_MAX) {
            n = IO_MAX;
        }
        node->io_count = n;
        for (i = 0; i < n; i++) {
            node->io_pollers[i] = ntk_poller_pool_at(node->pool, i);
        }
        node->poller = n ? node->io_pollers[0] : NULL;
    }

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

    /*
     * 可选 handler 线程池：把注册为 *_worker 的业务 handler 移出 poller，
     * 避免单个阻塞 handler 卡死整条 poller。回复经 ntk_poller_async 回投。
     */
    if (cfg->handler_threads > 0) {
        ntk_thread_pool_opts_t hopts;
        memset(&hopts, 0, sizeof(hopts));
        hopts.thread_count = cfg->handler_threads;
        hopts.priority = NTK_THREAD_PRIO_NORMAL;
        hopts.auto_start = 1;
        node->handler_pool = ntk_thread_pool_create(&hopts);
        if (!node->handler_pool) {
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
            if (zri_node_transport_add(node, endpoint.scheme, endpoint.host, endpoint.port) != OK) {
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
    {
        unsigned i;
        for (i = 0; i < node->io_count; i++) {
            node->pending_timers[i] = ntk_timer_start(
                node->io_pollers[i], zri_tick_interval_ms(), 1, pending_timer_cb, node);
        }
    }
    node->started = 1;

    if (node->mode == MODE_MESH) {
        if (node->registry_backend && node->registry_backend->ops->announce) {
            node->registry_backend->ops->announce(node, 0);
        }
    }
    *out = node;
    return OK;
}

void node_destroy(node_t *node)
{
    if (!node) {
        return;
    }
    node->shutting_down = 1;
    if (node->started && node->mode == MODE_MESH) {
        if (node->registry_backend && node->registry_backend->ops->announce) {
            node->registry_backend->ops->announce(node, 1); /* BYE */
        }
    }
    /*
     * 先销毁 handler 线程池：drain 队列并 join worker。worker 期间的 reply 会
     * 回投仍运行的 poller，从而在传输/路由销毁前完成发送。
     */
    if (node->handler_pool) {
        ntk_thread_pool_destroy(node->handler_pool);
        node->handler_pool = NULL;
    }
    /* 池已 drain，worker task/call 已归还；此时销毁复用池。 */
    zri_obj_pool_destroy(&node->handler_task_pool);
    zri_obj_pool_destroy(&node->worker_call_pool);
    /* Quiesce poller callbacks before destroying sockets or transport state. */
    if (node->pool) {
        ntk_poller_pool_stop(node->pool);
        /*
         * 停线程后、拆传输前，排空回投的回复任务：此时仍在本线程执行发送，
         * 传输对象尚存活，避免任务在传输销毁后才被 drain 造成悬垂访问。
         */
        {
            unsigned pi;
            unsigned pn = ntk_poller_pool_size(node->pool);
            for (pi = 0; pi < pn; pi++) {
                ntk_poller_process_pending(ntk_poller_pool_at(node->pool, pi));
            }
        }
    }
    /* 回投的回复任务已排空，任务头可安全回收。 */
    zri_obj_pool_destroy(&node->reply_task_pool);
    /*
     * poller 已停：取消登记/限流计数不再被定时器或 worker 访问，可以安全销毁。
     * 内置池已 drain 并 join，inflight 归零；自定义 executor 由调用者负责排空，
     * 这里只做诊断，若仍有任务在飞说明契约被违反。
     */
    if (node->handler_limit_mutex) {
        ntk_mutex_lock(node->handler_limit_mutex);
        if (node->external_inflight > 0) {
            rpc_log_error("node_destroy: %u custom-executor task(s) still in flight; "
                          "caller must drain the executor before destroy",
                          node->external_inflight);
        }
        ntk_mutex_unlock(node->handler_limit_mutex);
        ntk_mutex_destroy(node->handler_limit_mutex);
        node->handler_limit_mutex = NULL;
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
        unsigned i;
        for (i = 0; i < IO_MAX; i++) {
            if (node->pending_timers[i]) {
                ntk_timer_stop(node->pending_timers[i]);
                node->pending_timers[i] = NULL;
            }
        }
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
    zri_pending_cleanup(node);
    if (node->pending_lock) {
        ntk_mutex_destroy(node->pending_lock);
        node->pending_lock = NULL;
    }
    if (node->rate_lock) {
        ntk_mutex_destroy(node->rate_lock);
        node->rate_lock = NULL;
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
    if (node->services_lock) {
        ntk_mutex_destroy(node->services_lock);
        node->services_lock = NULL;
    }
    if (node->codecs_lock) {
        ntk_mutex_destroy(node->codecs_lock);
        node->codecs_lock = NULL;
    }
    {
        int i;
        for (i = 0; i < MAX_RECV_STREAMS; i++) {
            if (node->recv_streams[i].used && node->recv_streams[i].call) {
                zri_mem_free(node->recv_streams[i].call);
                node->recv_streams[i].call = NULL;
                node->recv_streams[i].used = 0;
            }
            free(node->recv_streams[i].metadata);
            node->recv_streams[i].metadata = NULL;
        }
    }
    free(node);
}

/* ---- 节点状态与端点 ---- */

const char *node_name(const node_t *node)
{
    return node ? node->name : NULL;
}

transport_kind_t node_transport(const node_t *node)
{
    return (node && node->transport_count > 0 && node->transports[0]) ? node->transports[0]->kind :
                                                                        TRANSPORT_UDP;
}

uint16_t node_discovery_port(const node_t *node)
{
    return node ? node->discovery_port : 0;
}

uint16_t node_data_port(const node_t *node)
{
    return (node && node->transport_count > 0 && node->transports[0]) ? node->transports[0]->port :
                                                                        0;
}

size_t node_binding_count(const node_t *node)
{
    return node ? node->transport_count : 0;
}

int node_binding_endpoint(const node_t *node, size_t index, char *out, size_t cap)
{
    endpoint_t endpoint;
    if (!node || index >= node->transport_count || !node->transports[index]) {
        return ERR_NOTFOUND;
    }
    memset(&endpoint, 0, sizeof(endpoint));
    zri_copy_str(endpoint.scheme, sizeof(endpoint.scheme), node->transports[index]->scheme, "udp");
    zri_copy_str(
        endpoint.host, sizeof(endpoint.host), node->transports[index]->bind_host, "0.0.0.0");
    endpoint.port = node->transports[index]->port;
    endpoint.transport = node->transports[index]->kind;
    return endpoint_format(&endpoint, out, cap);
}

/* ---- 指标与对端质量 ---- */

static const uint32_t g_latency_bounds[LATENCY_HIST_BUCKETS - 1] = {
    1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000
};

/* 从累计直方图估算分位值；最后一桶返回 10000（表示 >=10s）。 */
static uint64_t latency_percentile(const uint64_t *hist, uint64_t total, uint32_t pct)
{
    uint64_t target;
    uint64_t cumulative = 0;
    int i;
    if (!hist || total == 0) {
        return 0;
    }
    target = (total * (uint64_t)pct + 99u) / 100u;
    for (i = 0; i < LATENCY_HIST_BUCKETS; i++) {
        cumulative += hist[i];
        if (cumulative >= target) {
            return i < LATENCY_HIST_BUCKETS - 1 ? g_latency_bounds[i] : 10000u;
        }
    }
    return 10000u;
}

void node_metrics(const node_t *node, metrics_t *out)
{
    unsigned i;
    if (!out) {
        return;
    }
    if (!node) {
        memset(out, 0, sizeof(*out));
        return;
    }
    /* 逐字段原子读，避免与多个 poller 的增量并发时读到撕裂值。 */
#define ZRI_METRICS_SNAPSHOT_U64(field) \
    out->field = zri_atomic_load_u64(&node->metrics.field)
    ZRI_METRICS_SNAPSHOT_U64(requests_sent);
    ZRI_METRICS_SNAPSHOT_U64(responses_received);
    ZRI_METRICS_SNAPSHOT_U64(requests_failed);
    ZRI_METRICS_SNAPSHOT_U64(timeouts);
    ZRI_METRICS_SNAPSHOT_U64(retries);
    ZRI_METRICS_SNAPSHOT_U64(bytes_sent);
    ZRI_METRICS_SNAPSHOT_U64(bytes_received);
    ZRI_METRICS_SNAPSHOT_U64(udp_packets_sent);
    ZRI_METRICS_SNAPSHOT_U64(udp_packets_received);
    ZRI_METRICS_SNAPSHOT_U64(udp_retransmissions);
    ZRI_METRICS_SNAPSHOT_U64(udp_nacks_sent);
    ZRI_METRICS_SNAPSHOT_U64(udp_reassembly_expired);
    ZRI_METRICS_SNAPSHOT_U64(tcp_frames_sent);
    ZRI_METRICS_SNAPSHOT_U64(tcp_frames_received);
    ZRI_METRICS_SNAPSHOT_U64(backpressure_events);
    ZRI_METRICS_SNAPSHOT_U64(rtcp_sr_sent);
    ZRI_METRICS_SNAPSHOT_U64(rtcp_rr_sent);
    ZRI_METRICS_SNAPSHOT_U64(rtcp_sr_received);
    ZRI_METRICS_SNAPSHOT_U64(rtcp_rr_received);
    ZRI_METRICS_SNAPSHOT_U64(rtcp_bye_sent);
    ZRI_METRICS_SNAPSHOT_U64(rtcp_bye_received);
    out->rtcp_rtt_ms = zri_atomic_load_u32(&node->metrics.rtcp_rtt_ms);
    out->rtcp_jitter_ms = zri_atomic_load_u32(&node->metrics.rtcp_jitter_ms);
    out->rtcp_remote_lost =
        (int32_t)zri_atomic_load_u32((const uint32_t *)&node->metrics.rtcp_remote_lost);
    ZRI_METRICS_SNAPSHOT_U64(connections);
    ZRI_METRICS_SNAPSHOT_U64(latency_count);
    ZRI_METRICS_SNAPSHOT_U64(latency_sum_ms);
    ZRI_METRICS_SNAPSHOT_U64(rate_limited);
    ZRI_METRICS_SNAPSHOT_U64(streams_rejected);
    for (i = 0; i < LATENCY_HIST_BUCKETS; i++) {
        out->latency_hist[i] = zri_atomic_load_u64(&node->metrics.latency_hist[i]);
    }
#undef ZRI_METRICS_SNAPSHOT_U64
    out->latency_p50_ms = latency_percentile(out->latency_hist, out->latency_count, 50u);
    out->latency_p90_ms = latency_percentile(out->latency_hist, out->latency_count, 90u);
    out->latency_p99_ms = latency_percentile(out->latency_hist, out->latency_count, 99u);
    out->inflight_requests = zri_atomic_load_u32((const uint32_t *)&node->pending_count);
    if (node->handler_limit_mutex) {
        ntk_mutex_lock(node->handler_limit_mutex);
        out->inflight_handlers = (uint64_t)node->handler_inflight + node->external_inflight;
        ntk_mutex_unlock(node->handler_limit_mutex);
    }
}

int node_peer_stats(const node_t *node, const char *ip, uint16_t port, peer_stats_t *out)
{
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

/* ---- 发现与路由 ---- */

int node_add_discovery_peer(node_t *node, const char *ip, uint16_t discovery_port)
{
    int i;
    if (!node || !ip || !discovery_port) {
        return ERR_INVALID;
    }
    for (i = 0; i < DISC_TARGET_MAX; i++) {
        if (!node->discovery.targets[i].used) {
            node->discovery.targets[i].used = 1;
            zri_copy_str(node->discovery.targets[i].ip,
                         sizeof(node->discovery.targets[i].ip),
                         ip,
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

int node_add_static_route(
    node_t *node, const char *service, const char *ip, uint16_t port, transport_kind_t transport)
{
    return zri_route_set(&node->routes, service, ip, port, transport);
}

int node_add_static_endpoint(node_t *node, const char *service, const char *endpoint)
{
    return zri_route_add_endpoint(&node->routes, service, endpoint, 1);
}

/* ---- 服务等待与调度 ---- */

int node_wait_for_service(node_t *node, const char *service, uint64_t timeout_ms)
{
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
        if (zri_route_lookup(&node->routes, service, ip, sizeof(ip), &port, NULL, NULL, 0) == OK) {
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

static void post_trampoline(void *user)
{
    zri_post_task_t *t = (zri_post_task_t *)user;
    t->fn(t->user);
    zri_mem_free(t);
}

int node_post(node_t *node, void (*fn)(void *user), void *user)
{
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

static uint64_t post_delay_cb(void *user)
{
    zri_post_task_t *t = (zri_post_task_t *)user;
    t->fn(t->user);
    zri_mem_free(t);
    return 0; /* 单次 */
}

int node_post_delay(node_t *node, uint64_t delay_ms, void (*fn)(void *user), void *user)
{
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

static void zri_signal_handler(int signo)
{
    (void)signo;
    g_signal = 1;
}

int node_spin_once(node_t *node, uint64_t timeout_ms)
{
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

int node_spin(node_t *node)
{
    return node_spin_once(node, 0);
}
