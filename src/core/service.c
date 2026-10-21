/* 服务端：方法注册与请求分发（回复见 reply.c）。 */
#include "node_internal.h"
#include "service_internal.h"

#include "envelope.h"
#include "atomic.h"
#include "mem.h"
#include "util.h"

#include <log.h>

#include <stdlib.h>
#include <string.h>

/* 同步风格 handler 的桥接：返回值即状态码，把视图回复后再释放。 */
typedef struct zri_sync_bridge {
    sync_handler_fn fn;
    void *user;
} zri_sync_bridge_t;

static void sync_method_bridge(call_t *call, const request_t *req, void *user)
{
    zri_sync_bridge_t *bridge = (zri_sync_bridge_t *)user;
    message_view_t view;
    payload_t pl;
    int status;
    message_view_init(&view);
    status = bridge->fn(req, &view, bridge->user);
    message_view_to_payload(&view, &pl);
    reply(call, status, &pl);
    message_view_release(&view);
}

typedef struct zri_svc_task {
    node_t *node;
    service_t *svc;
    zri_method_t *method;
} zri_svc_task_t;

typedef struct zri_announce_task {
    node_t *node;
} zri_announce_task_t;

static void announce_task_fn(void *user)
{
    zri_announce_task_t *t = (zri_announce_task_t *)user;
    if (t) {
        if (t->node && t->node->registry_backend && t->node->registry_backend->ops->announce) {
            t->node->registry_backend->ops->announce(t->node, 0);
        }
        free(t);
    }
}

static void svc_link_task(void *user)
{
    zri_svc_task_t *t = (zri_svc_task_t *)user;
    ntk_mutex_lock(t->node->services_lock);
    t->svc->next = t->node->services;
    t->node->services = t->svc;
    ntk_mutex_unlock(t->node->services_lock);
    if (t->node->registry_backend && t->node->registry_backend->ops->announce) {
        t->node->registry_backend->ops->announce(t->node, 0);
    }
    free(t);
}

static void svc_method_task(void *user)
{
    zri_svc_task_t *t = (zri_svc_task_t *)user;
    ntk_mutex_lock(t->node->services_lock);
    t->method->next = t->svc->methods;
    t->svc->methods = t->method;
    ntk_mutex_unlock(t->node->services_lock);
    free(t);
}

/*
 * 阻塞 handler 的线程池任务：请求字段全部深拷贝。worker 里构造 call 并执行
 * 用户 handler；reply() 会自动把响应回投到 origin poller 发送。
 */
#define HANDLER_INLINE_PAYLOAD 128

typedef struct zri_handler_task {
    node_t *node;
    zri_transport_t *transport;
    zri_peer_token_t token;
    ntk_poller *poller;
    char peer_ip[ENDPOINT_MAX];
    uint16_t peer_port;
    uint32_t msg_id;
    int one_way;
    handler_fn fn;
    void *user;
    service_interceptor_fn interceptor;
    void *interceptor_user;
    char service[SERVICE_NAME_MAX];
    char method[METHOD_NAME_MAX];
    uint8_t *payload;
    uint8_t inline_payload[HANDLER_INLINE_PAYLOAD]; /* 小请求免一次 malloc */
    size_t payload_len;
    uint8_t encoding;
    uint64_t deadline_ms; /* 请求 deadline（绝对单调 ms，0 = 无） */
    metadata_t *metadata; /* 请求 metadata 解码副本，按需分配；无则为 NULL */
    int external;         /* 1 = 自定义 executor 任务 */
} zri_handler_task_t;

static void handler_inflight_release(node_t *node);
static void handler_external_release(node_t *node);

static void handler_task_release(zri_handler_task_t *t)
{
    if (t->external) {
        handler_inflight_release(t->node);
        handler_external_release(t->node);
    } else {
        handler_inflight_release(t->node);
    }
}

static void handler_task_destroy(zri_handler_task_t *t)
{
    if (!t) {
        return;
    }
    if (t->payload && t->payload != t->inline_payload) {
        free(t->payload);
    }
    free(t->metadata);
    if (t->node && t->node->handler_task_pool.lock) {
        zri_obj_pool_free(&t->node->handler_task_pool, t);
    } else {
        free(t);
    }
}

/* worker 路径的 call：优先取节点复用池，抢不到锁/池空则回退 malloc。 */
call_t *zri_worker_call_alloc(node_t *node)
{
    call_t *call;
    if (!node) {
        return NULL;
    }
    call = (call_t *)zri_obj_pool_alloc(&node->worker_call_pool, sizeof(*call));
    if (!call) {
        return NULL;
    }
    memset(call, 0, sizeof(*call));
    call->from_worker_pool = 1;
    return call;
}

void zri_worker_call_free(node_t *node, call_t *call)
{
    if (!call) {
        return;
    }
    if (call->from_worker_pool) {
        if (node && node->worker_call_pool.lock) {
            zri_obj_pool_free(&node->worker_call_pool, call);
        } else {
            free(call);
        }
        return;
    }
    zri_mem_free(call);
}

static void handler_task_run(void *user)
{
    zri_handler_task_t *t = (zri_handler_task_t *)user;
    call_t *call;
    request_t req;
    int one_way = t->one_way;

    /* worker 线程没有 current poller，zri_mem_alloc 会回退到 malloc。 */
    call = zri_worker_call_alloc(t->node);
    if (!call) {
        handler_task_release(t);
        handler_task_destroy(t);
        return;
    }
    /* zri_worker_call_alloc 已清零并设置 from_worker_pool，这里不能再 memset。 */
    call->node = t->node;
    call->transport = t->transport;
    call->poller = t->poller;
    call->token = t->token;
    zri_copy_str(call->peer_ip, sizeof(call->peer_ip), t->peer_ip, "0.0.0.0");
    call->peer_port = t->peer_port;
    call->msg_id = t->msg_id;
    call->one_way = one_way;
    call->deadline_ms = t->deadline_ms;

    req.service = t->service;
    req.method = t->method;
    req.payload.data = t->payload;
    req.payload.len = t->payload_len;
    req.payload.encoding = t->encoding;
    req.peer_ip = call->peer_ip;
    req.peer_port = call->peer_port;
    req.deadline_ms = t->deadline_ms;
    req.metadata = t->metadata;

    /*
     * 排队期间可能已过期或被客户端取消：启动 handler 前再检查一次，避免做
     * 注定被丢弃的重活。取消/超时的响应客户端通常已不再等待，发送失败也无妨。
     */
    {
        int canceled = zri_node_is_canceled(t->node, t->msg_id, t->peer_ip, t->peer_port);
        int expired = t->deadline_ms != 0 && zri_now_ms() >= t->deadline_ms;
        if (canceled || expired) {
            if (!one_way) {
                reply(call, canceled ? ERR_NOTFOUND : ERR_TIMEOUT, NULL);
            } else {
                zri_worker_call_free(t->node, call);
            }
            handler_task_release(t);
            handler_task_destroy(t);
            return;
        }
    }
    if (t->interceptor) {
        int st = t->interceptor(call, &req, t->interceptor_user);
        if (st != OK) {
            if (!one_way) {
                reply(call, st, NULL);
            } else {
                zri_worker_call_free(t->node, call);
            }
            handler_task_release(t);
            handler_task_destroy(t);
            return;
        }
    }
    rpc_log_debug("worker request %s.%s id=%llu len=%zu",
                  t->service,
                  t->method,
                  (unsigned long long)t->msg_id,
                  t->payload_len);

    t->fn(call, &req, t->user);
    handler_task_release(t);
    /* one_way 没有 reply，call 在 handler 返回后释放；请求载荷也可释放。 */
    if (one_way) {
        zri_worker_call_free(t->node, call);
    }
    handler_task_destroy(t);
}

/*
 * worker 未完成任务（排队 + 执行中）限流。0 表示不限制；超限返回 ERR_AGAIN，
 * 由调用方在 poller 上直接回错误状态，避免无界排队。
 */
static int handler_inflight_acquire(node_t *node)
{
    int ok;
    if (node->handler_max_concurrency == 0 || !node->handler_limit_mutex) {
        return OK;
    }
    ntk_mutex_lock(node->handler_limit_mutex);
    ok = node->handler_inflight < node->handler_max_concurrency;
    if (ok) {
        node->handler_inflight++;
    }
    ntk_mutex_unlock(node->handler_limit_mutex);
    return ok ? OK : ERR_AGAIN;
}

static void handler_inflight_release(node_t *node)
{
    if (node->handler_max_concurrency == 0 || !node->handler_limit_mutex) {
        return;
    }
    ntk_mutex_lock(node->handler_limit_mutex);
    if (node->handler_inflight > 0) {
        node->handler_inflight--;
    }
    ntk_mutex_unlock(node->handler_limit_mutex);
}

static void handler_external_acquire(node_t *node)
{
    if (!node->handler_limit_mutex) {
        return;
    }
    ntk_mutex_lock(node->handler_limit_mutex);
    node->external_inflight++;
    ntk_mutex_unlock(node->handler_limit_mutex);
}

static void handler_external_release(node_t *node)
{
    if (!node->handler_limit_mutex) {
        return;
    }
    ntk_mutex_lock(node->handler_limit_mutex);
    if (node->external_inflight > 0) {
        node->external_inflight--;
    }
    ntk_mutex_unlock(node->handler_limit_mutex);
}

/* ---- 在途调用取消登记（best-effort） ---- */

void zri_node_cancel_record(node_t *node,
                            uint32_t request_id,
                            const char *peer_ip,
                            uint16_t peer_port)
{
    int i;
    int free_slot = -1;
    uint64_t now;
    if (!node || !node->handler_limit_mutex) {
        return;
    }
    now = zri_now_ms();
    ntk_mutex_lock(node->handler_limit_mutex);
    for (i = 0; i < CANCEL_SLOTS; i++) {
        zri_cancel_slot_t *s = &node->cancels[i];
        if (s->used && s->expires_ms <= now) {
            s->used = 0;
            if (node->cancel_count > 0) {
                zri_atomic_sub_u32(&node->cancel_count, 1u);
            }
        }
        if (!s->used) {
            if (free_slot < 0) {
                free_slot = i;
            }
            continue;
        }
        /* 同一调用的重复取消：刷新 TTL 即可。 */
        if (s->request_id == request_id && s->peer_port == peer_port &&
            strcmp(s->peer_ip, peer_ip ? peer_ip : "") == 0) {
            s->expires_ms = now + CANCEL_TTL_MS;
            ntk_mutex_unlock(node->handler_limit_mutex);
            return;
        }
    }
    if (free_slot >= 0) {
        zri_cancel_slot_t *s = &node->cancels[free_slot];
        s->used = 1;
        s->request_id = request_id;
        zri_copy_str(s->peer_ip, sizeof(s->peer_ip), peer_ip, "0.0.0.0");
        s->peer_port = peer_port;
        s->expires_ms = now + CANCEL_TTL_MS;
        zri_atomic_add_u32(&node->cancel_count, 1u);
    }
    ntk_mutex_unlock(node->handler_limit_mutex);
}

int zri_node_is_canceled(const node_t *node,
                         uint32_t request_id,
                         const char *peer_ip,
                         uint16_t peer_port)
{
    int found = 0;
    int i;
    uint64_t now;
    if (!node || !node->handler_limit_mutex ||
        zri_atomic_load_u32(&node->cancel_count) == 0) {
        return 0;
    }
    now = zri_now_ms();
    ntk_mutex_lock(node->handler_limit_mutex);
    for (i = 0; i < CANCEL_SLOTS; i++) {
        const zri_cancel_slot_t *s = &node->cancels[i];
        if (s->used && s->expires_ms > now && s->request_id == request_id &&
            s->peer_port == peer_port && strcmp(s->peer_ip, peer_ip ? peer_ip : "") == 0) {
            found = 1;
            break;
        }
    }
    ntk_mutex_unlock(node->handler_limit_mutex);
    return found;
}

void zri_node_expire_cancels(node_t *node, uint64_t now_ms)
{
    int i;
    if (!node || !node->handler_limit_mutex) {
        return;
    }
    ntk_mutex_lock(node->handler_limit_mutex);
    for (i = 0; i < CANCEL_SLOTS; i++) {
        zri_cancel_slot_t *s = &node->cancels[i];
        if (s->used && s->expires_ms <= now_ms) {
            s->used = 0;
            if (node->cancel_count > 0) {
                zri_atomic_sub_u32(&node->cancel_count, 1u);
            }
        }
    }
    ntk_mutex_unlock(node->handler_limit_mutex);
}

/* 由客户端 timeout_ms 换算成绝对 deadline（接收时刻起算）。 */
static uint64_t deadline_from_timeout(uint32_t timeout_ms)
{
    return timeout_ms ? zri_now_ms() + timeout_ms : 0;
}

/* 把已解析好的请求深拷贝成可跨线程执行的任务。 */
static int handler_task_create(node_t *node,
                               zri_transport_t *transport,
                               const zri_message_t *msg,
                               const char *service,
                               const char *method,
                               handler_fn fn,
                               void *user,
                               service_interceptor_fn interceptor,
                               void *interceptor_user,
                               int external,
                               zri_handler_task_t **out)
{
    zri_handler_task_t *t;
    size_t payload_len = msg->payload.len;

    *out = NULL;
    if (payload_len > 0 && !msg->payload.data) {
        return ERR_INVALID;
    }
    t = (zri_handler_task_t *)zri_obj_pool_alloc(&node->handler_task_pool, sizeof(*t));
    if (!t) {
        return ERR_NOMEM;
    }
    memset(t, 0, sizeof(*t));
    t->node = node;
    t->transport = transport;
    t->token = msg->peer_token;
    t->poller = ntk_poller_current();
    zri_copy_str(t->peer_ip, sizeof(t->peer_ip), msg->peer_ip, "0.0.0.0");
    t->peer_port = msg->peer_port;
    t->msg_id = msg->request_id;
    t->one_way = msg->kind == KIND_EVENT;
    t->fn = fn;
    t->user = user;
    t->interceptor = interceptor;
    t->interceptor_user = interceptor_user;
    t->external = external;
    zri_copy_str(t->service, sizeof(t->service), service, "");
    zri_copy_str(t->method, sizeof(t->method), method, "");
    t->encoding = msg->payload.encoding;
    t->deadline_ms = deadline_from_timeout(msg->timeout_ms);
    if (msg->metadata_len > 0 && msg->metadata) {
        metadata_t *md = (metadata_t *)malloc(sizeof(*md));
        if (md) {
            if (metadata_decode(md, msg->metadata, msg->metadata_len) == OK && md->count > 0) {
                t->metadata = md;
            } else {
                free(md);
            }
        }
    }
    t->payload_len = payload_len;
    if (payload_len > 0) {
        t->payload = payload_len <= sizeof(t->inline_payload) ? t->inline_payload :
                                                                (uint8_t *)malloc(payload_len);
        if (!t->payload) {
            handler_task_destroy(t);
            return ERR_NOMEM;
        }
        memcpy(t->payload, msg->payload.data, payload_len);
    }
    *out = t;
    return OK;
}

/* 投递到节点内置 handler 线程池。 */
static int service_dispatch_worker(node_t *node,
                                   zri_transport_t *transport,
                                   const zri_message_t *msg,
                                   const char *service,
                                   const char *method,
                                   handler_fn fn,
                                   void *user,
                                   service_interceptor_fn interceptor,
                                   void *interceptor_user)
{
    zri_handler_task_t *t = NULL;
    int rc;

    rc = handler_inflight_acquire(node);
    if (rc != OK) {
        return rc;
    }
    rc = handler_task_create(
        node, transport, msg, service, method, fn, user, interceptor, interceptor_user, 0, &t);
    if (rc != OK) {
        handler_inflight_release(node);
        return rc;
    }
    if (ntk_thread_pool_async(node->handler_pool, handler_task_run, t, 1) != NTK_OK) {
        handler_inflight_release(node);
        handler_task_destroy(t);
        return ERR_IO;
    }
    return OK;
}

/* 投递到调用者提供的 executor（借用语义）。 */
static int service_dispatch_external(node_t *node,
                                     zri_transport_t *transport,
                                     const zri_message_t *msg,
                                     const char *service,
                                     const char *method,
                                     handler_fn fn,
                                     void *user,
                                     const service_executor_t *executor,
                                     service_interceptor_fn interceptor,
                                     void *interceptor_user)
{
    zri_handler_task_t *t = NULL;
    int rc;

    /* 自定义 executor 与内置 worker 共用同一未完成上限，避免执行器侧无界排队。 */
    rc = handler_inflight_acquire(node);
    if (rc != OK) {
        return rc;
    }
    rc = handler_task_create(
        node, transport, msg, service, method, fn, user, interceptor, interceptor_user, 1, &t);
    if (rc != OK) {
        handler_inflight_release(node);
        return rc;
    }
    handler_external_acquire(node);
    rc = executor->submit(executor->ctx, handler_task_run, t);
    if (rc != OK) {
        handler_external_release(node);
        handler_inflight_release(node);
        handler_task_destroy(t);
        return rc;
    }
    return OK;
}

void zri_service_handle_request(node_t *node, zri_transport_t *transport, const zri_message_t *msg)
{
    char tmp[MAX_ROUTE];
    char *dot;
    service_t *svc;
    zri_method_t *m = NULL;
    handler_fn fn = NULL;
    void *user = NULL;
    service_interceptor_fn interceptor = NULL;
    void *interceptor_user = NULL;
    int dedup_active = 0;

    if (!node || !msg) {
        return;
    }
    zri_copy_str(tmp, sizeof(tmp), msg->route, "");
    dot = strchr(tmp, '.');
    if (!dot) {
        zri_service_send_status(transport, msg, ERR_NOTFOUND);
        return;
    }
    *dot = '\0';
    ntk_mutex_lock(node->services_lock);
    for (svc = node->services; svc; svc = svc->next) {
        if (svc->removed || strcmp(svc->name, tmp) != 0) {
            continue;
        }
        for (m = svc->methods; m; m = m->next) {
            if (strcmp(m->name, dot + 1) == 0 && m->fn) {
                fn = m->fn;
                user = m->user;
                break;
            }
        }
        if (fn) {
            interceptor = svc->interceptor;
            interceptor_user = svc->interceptor_user;
        }
        break;
    }
    ntk_mutex_unlock(node->services_lock);
    if (!fn) {
        zri_service_send_status(transport, msg, ERR_NOTFOUND);
        return;
    }
    /*
     * UDP unary 去重：命中已完成请求直接重放缓存响应；命中在途请求丢弃重复，
     * 避免非幂等 handler 被重传执行多次。TCP 可靠，不参与。
     */
    if (transport && transport->kind == TRANSPORT_UDP && msg->kind == KIND_REQUEST &&
        msg->request_id != 0) {
        int dstate = 0;
        (void)zri_dedup_begin(&node->dedup, transport, msg, &dstate);
        if (dstate == 1) {
            return;
        }
        if (dstate == 2) {
            return;
        }
        dedup_active = 1;
    }
    /*
     * WORKER 方法走节点线程池。池未配置时退化为内联并告警；入池失败时直接
     * 回错误状态，避免慢 handler 在池异常时阻塞 poller。
     */
    if (m->executor) {
        int rc = service_dispatch_external(node,
                                           transport,
                                           msg,
                                           tmp,
                                           dot + 1,
                                           fn,
                                           user,
                                           m->executor,
                                           interceptor,
                                           interceptor_user);
        if (rc == OK) {
            return;
        }
        rpc_log_error("executor dispatch failed: service=%s method=%s rc=%d", tmp, dot + 1, rc);
        if (dedup_active) {
            zri_dedup_abort(&node->dedup, msg);
        }
        zri_service_send_status(transport, msg, rc);
        return;
    }
    if (m->run_in_worker) {
        if (node->handler_pool) {
            int rc = service_dispatch_worker(node,
                                             transport,
                                             msg,
                                             tmp,
                                             dot + 1,
                                             fn,
                                             user,
                                             interceptor,
                                             interceptor_user);
            if (rc == OK) {
                return;
            }
            if (rc == ERR_AGAIN) {
                rpc_log_warn("worker overloaded: service=%s method=%s", tmp, dot + 1);
            } else {
                rpc_log_error(
                    "worker dispatch failed: service=%s method=%s rc=%d", tmp, dot + 1, rc);
            }
            if (dedup_active) {
                zri_dedup_abort(&node->dedup, msg);
            }
            zri_service_send_status(transport, msg, rc);
            return;
        }
        if (zri_atomic_exchange_u32((uint32_t *)&node->worker_fallback_warned, 1u) == 0) {
            rpc_log_warn(
                "handler_threads=0: worker method %s.%s runs inline on poller", tmp, dot + 1);
        }
    }
    rpc_log_debug("request %s.%s id=%llu len=%zu peer=%s:%u",
                  tmp,
                  dot + 1,
                  (unsigned long long)msg->request_id,
                  msg->payload.len,
                  msg->peer_ip ? msg->peer_ip : "?",
                  (unsigned)msg->peer_port);
    {
        call_t *call = (call_t *)zri_mem_alloc(sizeof(*call));
        request_t req;
        metadata_t request_metadata;
        int has_metadata = 0;
        if (!call) {
            if (dedup_active) {
                zri_dedup_abort(&node->dedup, msg);
            }
            zri_service_send_status(transport, msg, ERR_NOMEM);
            return;
        }
        memset(call, 0, sizeof(*call));
        call->node = node;
        call->transport = transport;
        call->poller = ntk_poller_current();
        call->token = msg->peer_token;
        zri_copy_str(call->peer_ip, sizeof(call->peer_ip), msg->peer_ip, "0.0.0.0");
        call->peer_port = msg->peer_port;
        call->msg_id = msg->request_id;
        call->one_way = msg->kind == KIND_EVENT;
        call->deadline_ms = deadline_from_timeout(msg->timeout_ms);
        req.service = tmp;
        req.method = dot + 1;
        req.payload = msg->payload;
        req.peer_ip = call->peer_ip;
        req.peer_port = msg->peer_port;
        req.deadline_ms = call->deadline_ms;
        if (msg->metadata_len > 0 && msg->metadata &&
            metadata_decode(&request_metadata, msg->metadata, msg->metadata_len) == OK &&
            request_metadata.count > 0) {
            has_metadata = 1;
        }
        req.metadata = has_metadata ? &request_metadata : NULL;
        {
            int one_way = call->one_way;
            int st = interceptor ? interceptor(call, &req, interceptor_user) : OK;
            if (st == OK) {
                fn(call, &req, user);
            } else {
                reply(call, st, NULL);
            }
            if (one_way) {
                zri_mem_free(call);
            }
        }
    }
}

/*
 * 客户端取消：按 request_id + 对端登记。worker 任务启动前会查表跳过；
 * 已运行的 handler 可通过 call_canceled() 观测（无法抢占）。
 */
void zri_service_handle_cancel(node_t *node, zri_transport_t *transport, const zri_message_t *msg)
{
    (void)transport;
    if (!node || !msg || msg->kind != KIND_CANCEL) {
        return;
    }
    zri_node_cancel_record(node, msg->request_id, msg->peer_ip, msg->peer_port);
}

uint64_t call_deadline_ms(const call_t *call)
{
    return call ? call->deadline_ms : 0;
}

int call_canceled(const call_t *call)
{
    if (!call) {
        return 0;
    }
    return zri_node_is_canceled(call->node, call->msg_id, call->peer_ip, call->peer_port);
}

static zri_recv_stream_t *find_recv_stream(node_t *node, const zri_message_t *msg)
{
    int i;
    for (i = 0; i < MAX_RECV_STREAMS; i++) {
        zri_recv_stream_t *stream = &node->recv_streams[i];
        if (stream->used && stream->request_id == msg->request_id &&
            stream->peer_port == msg->peer_port &&
            strcmp(stream->peer_ip, msg->peer_ip ? msg->peer_ip : "") == 0) {
            return stream;
        }
    }
    return NULL;
}

static zri_recv_stream_t *alloc_recv_stream(node_t *node,
                                            zri_transport_t *transport,
                                            const zri_message_t *msg)
{
    int i;
    for (i = 0; i < MAX_RECV_STREAMS; i++) {
        zri_recv_stream_t *stream = &node->recv_streams[i];
        if (!stream->used) {
            call_t *call = (call_t *)zri_mem_alloc(sizeof(*call));
            if (!call) {
                return NULL;
            }
            memset(call, 0, sizeof(*call));
            stream->used = 1;
            stream->request_id = msg->request_id;
            zri_copy_str(stream->peer_ip, sizeof(stream->peer_ip), msg->peer_ip, "0.0.0.0");
            stream->peer_port = msg->peer_port;
            stream->call = call;
            stream->last_ms = zri_now_ms();
            stream->bytes = 0;
            call->node = node;
            call->transport = transport;
            call->poller = ntk_poller_current();
            call->token = msg->peer_token;
            zri_copy_str(call->peer_ip, sizeof(call->peer_ip), msg->peer_ip, "0.0.0.0");
            call->peer_port = msg->peer_port;
            call->msg_id = msg->request_id;
            call->stream_owned = 1;
            call->deadline_ms = deadline_from_timeout(msg->timeout_ms);
            return stream;
        }
    }
    return NULL;
}

/* ---- 流式 chunk 的 worker/executor 派发（每个 stream 串行，保证顺序） ---- */

typedef struct zri_stream_task {
    zri_recv_stream_t *state;
    call_t *call;
    uint8_t *data;
    size_t len;
    uint8_t encoding;
    unsigned flags;
    uint64_t offset;
    const metadata_t *metadata; /* 借用 stream 状态里的副本；仅本次任务内有效 */
    uint64_t deadline_ms;
    service_interceptor_fn interceptor;
    void *interceptor_user;
    chunk_handler_fn fn;
    void *user;
    int first;
    int rejected;
    struct zri_stream_task *next;
} zri_stream_task_t;

static void stream_task_run(void *user);
static void stream_task_complete(void *user);

static void stream_task_free(zri_stream_task_t *t)
{
    if (!t) {
        return;
    }
    free(t->data);
    free(t);
}

static void stream_clear_pending(zri_recv_stream_t *state)
{
    zri_stream_task_t *t = state->pending_head;
    while (t) {
        zri_stream_task_t *next = t->next;
        stream_task_free(t);
        t = next;
    }
    state->pending_head = NULL;
    state->pending_tail = NULL;
}

static void stream_close(zri_recv_stream_t *state)
{
    call_t *call = state->call;
    stream_clear_pending(state);
    free(state->metadata);
    memset(state, 0, sizeof(*state));
    if (call) {
        zri_mem_free(call);
    }
}

static int stream_submit(zri_recv_stream_t *state, zri_stream_task_t *task)
{
    zri_method_t *m = state->method;
    node_t *node = state->call ? state->call->node : NULL;
    if (!m) {
        stream_task_free(task);
        return ERR_STATE;
    }
    if (m->executor) {
        if (m->executor->submit(m->executor->ctx, stream_task_run, task) != OK) {
            stream_task_free(task);
            return ERR_IO;
        }
        return OK;
    }
    if (node && node->handler_pool) {
        if (ntk_thread_pool_async(node->handler_pool, stream_task_run, task, 1) != NTK_OK) {
            stream_task_free(task);
            return ERR_IO;
        }
        return OK;
    }
    stream_task_free(task);
    return ERR_STATE;
}

static void stream_dispatch_next(zri_recv_stream_t *state)
{
    zri_stream_task_t *task;
    if (!state || state->in_flight) {
        return;
    }
    task = state->pending_head;
    if (!task) {
        return;
    }
    state->pending_head = task->next;
    if (!state->pending_head) {
        state->pending_tail = NULL;
    }
    task->next = NULL;
    state->in_flight = 1;
    if (stream_submit(state, task) != OK) {
        state->in_flight = 0;
        state->closing = 1;
    }
}

static void stream_task_run(void *user)
{
    zri_stream_task_t *task = (zri_stream_task_t *)user;
    zri_recv_stream_t *state = task->state;
    call_t *call = task->call;
    request_t req;
    chunk_t chunk;
    int rejected = 0;

    if (!state || !call) {
        stream_task_free(task);
        return;
    }
    req.service = state->service;
    req.method = state->method_name;
    req.payload.data = task->data;
    req.payload.len = task->len;
    req.payload.encoding = task->encoding;
    req.peer_ip = call->peer_ip;
    req.peer_port = call->peer_port;
    req.deadline_ms = task->deadline_ms;
    req.metadata = task->metadata;
    chunk.data = task->data;
    chunk.len = task->len;
    chunk.encoding = task->encoding;
    chunk.offset = task->offset;
    chunk.flags = task->flags;
    chunk.metadata = NULL;
    chunk.message = NULL;
    if (task->first && task->interceptor) {
        int st = task->interceptor(call, &req, task->interceptor_user);
        if (st != OK) {
            reply_stream(call, st, NULL);
            rejected = 1;
        }
    }
    if (!rejected) {
        task->fn(call, &req, &chunk, task->user);
    }
    task->rejected = rejected;
    if (ntk_poller_async(call->poller, stream_task_complete, task, 1) != NTK_OK) {
        stream_task_free(task);
    }
}

static void stream_task_complete(void *user)
{
    zri_stream_task_t *task = (zri_stream_task_t *)user;
    zri_recv_stream_t *state = task->state;
    int rejected = task->rejected;
    stream_task_free(task);
    if (!state || !state->used) {
        return;
    }
    state->in_flight = 0;
    state->last_ms = zri_now_ms();
    if (rejected) {
        state->closing = 1;
        stream_clear_pending(state);
    }
    if (state->closing && !state->pending_head) {
        stream_close(state);
        return;
    }
    stream_dispatch_next(state);
}

void zri_service_handle_stream_chunk(node_t *node,
                                     zri_transport_t *transport,
                                     const zri_message_t *msg)
{
    char tmp[MAX_ROUTE];
    char *dot;
    service_t *svc = NULL;
    zri_method_t *method = NULL;
    zri_recv_stream_t *state;
    request_t request;
    chunk_t chunk;
    const stream_meta_t *stream_meta;
    int use_worker;
    service_interceptor_fn interceptor = NULL;
    void *interceptor_user = NULL;

    if (!node || !msg || !msg->has_stream) {
        return;
    }
    stream_meta = &msg->stream;
    state = find_recv_stream(node, msg);

    if (stream_meta->flags & STREAM_CANCEL) {
        if (state) {
            if ((state->run_in_worker || state->executor) &&
                (state->in_flight || state->pending_head)) {
                state->closing = 1;
                state->canceled = 1;
                stream_clear_pending(state);
                if (!state->in_flight) {
                    stream_close(state);
                }
            } else {
                stream_close(state);
            }
        }
        return;
    }

    if (state) {
        method = state->method;
    } else {
        if (!(stream_meta->flags & STREAM_FIRST)) {
            return; /* 没有 FIRST 也没有已有状态：协议错误 */
        }
        zri_copy_str(tmp, sizeof(tmp), msg->route, "");
        dot = strchr(tmp, '.');
        if (!dot) {
            return;
        }
        *dot = '\0';
        ntk_mutex_lock(node->services_lock);
        for (svc = node->services; svc; svc = svc->next) {
            if (svc->removed || strcmp(svc->name, tmp) != 0) {
                continue;
            }
            for (method = svc->methods; method; method = method->next) {
                if (strcmp(method->name, dot + 1) == 0 && method->stream_fn) {
                    break;
                }
            }
            if (method) {
                interceptor = svc->interceptor;
                interceptor_user = svc->interceptor_user;
            }
            break;
        }
        ntk_mutex_unlock(node->services_lock);
        if (!method) {
            return;
        }
        state = alloc_recv_stream(node, transport, msg);
        if (state) {
            state->method = method;
            state->run_in_worker = method->run_in_worker;
            state->executor = method->executor;
            state->interceptor = interceptor;
            state->interceptor_user = interceptor_user;
            state->deadline_ms = state->call ? state->call->deadline_ms : 0;
            if (msg->metadata_len > 0 && msg->metadata) {
                metadata_t *md = (metadata_t *)calloc(1, sizeof(*md));
                if (md) {
                    if (metadata_decode(md, msg->metadata, msg->metadata_len) == OK &&
                        md->count > 0) {
                        state->metadata = md;
                    } else {
                        free(md);
                    }
                }
            }
            zri_copy_str(state->service, sizeof(state->service), tmp, "");
            zri_copy_str(state->method_name, sizeof(state->method_name), dot + 1, "");
        }
    }
    if (!state || !state->call || !method) {
        return;
    }
    use_worker = (state->run_in_worker && node->handler_pool) || state->executor;
    if (use_worker) {
        int first = !state->intercepted;
        zri_stream_task_t *task = (zri_stream_task_t *)calloc(1, sizeof(*task));
        if (!task) {
            state->closing = 1;
            stream_close(state);
            return;
        }
        task->state = state;
        task->call = state->call;
        task->fn = method->stream_fn;
        task->user = method->user;
        task->first = first;
        task->interceptor = state->interceptor;
        task->interceptor_user = state->interceptor_user;
        task->deadline_ms = state->deadline_ms;
        task->flags = stream_meta->flags;
        task->offset = stream_meta->offset;
        task->encoding = msg->payload.encoding;
        task->len = msg->payload.len;
        if (task->len > 0) {
            task->data = (uint8_t *)malloc(task->len);
            if (!task->data) {
                free(task);
                state->closing = 1;
                stream_close(state);
                return;
            }
            memcpy(task->data, msg->payload.data, task->len);
        }
        if (state->metadata) {
            task->metadata = state->metadata;
        }
        state->intercepted = 1;
        state->last_ms = zri_now_ms();
        state->bytes += msg->payload.len;
        if (state->pending_tail) {
            state->pending_tail->next = task;
        } else {
            state->pending_head = task;
        }
        state->pending_tail = task;
        if (stream_meta->flags & STREAM_LAST) {
            state->closing = 1;
        }
        stream_dispatch_next(state);
        if (state->closing && !state->in_flight && !state->pending_head) {
            stream_close(state);
        }
        return;
    }
    if (state->run_in_worker && !node->handler_pool &&
        zri_atomic_exchange_u32((uint32_t *)&node->worker_fallback_warned, 1u) == 0) {
        rpc_log_warn("handler_threads=0: worker stream %s.%s runs inline on poller",
                     state->service,
                     state->method_name);
    }
    state->last_ms = zri_now_ms();
    state->bytes += msg->payload.len;
    memset(&request, 0, sizeof(request));
    request.service = state->service;
    request.method = state->method_name;
    request.payload = msg->payload;
    request.peer_ip = state->call->peer_ip;
    request.peer_port = msg->peer_port;
    request.deadline_ms = state->call->deadline_ms;
    request.metadata = state->metadata;
    memset(&chunk, 0, sizeof(chunk));
    chunk.data = msg->payload.data;
    chunk.len = msg->payload.len;
    chunk.encoding = msg->payload.encoding;
    chunk.offset = stream_meta->offset;
    chunk.flags = stream_meta->flags;

    if (!state->intercepted && state->interceptor) {
        int st = state->interceptor(state->call, &request, state->interceptor_user);
        if (st != OK) {
            state->intercepted = 1;
            reply_stream(state->call, st, NULL);
            stream_close(state);
            return;
        }
    }
    state->intercepted = 1;
    method->stream_fn(state->call, &request, &chunk, method->user);

    if (stream_meta->flags & STREAM_LAST) {
        stream_close(state);
    }
}

void zri_service_expire_streams(node_t *node, uint64_t now_ms)
{
    int i;
    if (!node) {
        return;
    }
    for (i = 0; i < MAX_RECV_STREAMS; i++) {
        zri_recv_stream_t *stream = &node->recv_streams[i];
        if (stream->used && now_ms - stream->last_ms > STREAM_IDLE_TIMEOUT_MS) {
            if ((stream->run_in_worker || stream->executor) &&
                (stream->in_flight || stream->pending_head)) {
                stream->closing = 1;
                stream_clear_pending(stream);
                if (!stream->in_flight) {
                    stream_close(stream);
                }
            } else {
                stream_close(stream);
            }
        }
    }
}

/* ---- 服务生命周期 ---- */

int service_create(node_t *node, const char *service_name, service_t **out)
{
    service_t *svc;
    zri_svc_task_t *t;

    if (!node || !service_name || !service_name[0] || !out) {
        return ERR_INVALID;
    }
    *out = NULL;
    svc = (service_t *)calloc(1, sizeof(*svc));
    if (!svc) {
        return ERR_NOMEM;
    }
    svc->node = node;
    zri_copy_str(svc->name, sizeof(svc->name), service_name, "svc");
    t = (zri_svc_task_t *)calloc(1, sizeof(*t));
    if (!t) {
        free(svc);
        return ERR_NOMEM;
    }
    t->node = node;
    t->svc = svc;
    if (ntk_poller_async(node->poller, svc_link_task, t, 1) != NTK_OK) {
        free(t);
        free(svc);
        return ERR_IO;
    }
    *out = svc;
    return OK;
}

void service_destroy(service_t *svc)
{
    /*
     * 运行期摘除：置 removed 后不再匹配新请求（查找路径跳过）。方法与服务
     * 内存保留到 node_destroy 统一释放，避免在途 handler 仍持有 method 指针
     * 时被释放。announce 回投 poller，保持发现面一致（best-effort）。
     */
    if (!svc || !svc->node || !svc->node->services_lock) {
        return;
    }
    ntk_mutex_lock(svc->node->services_lock);
    svc->removed = 1;
    ntk_mutex_unlock(svc->node->services_lock);
    if (svc->node->poller && svc->node->registry_backend &&
        svc->node->registry_backend->ops->announce) {
        zri_announce_task_t *t = (zri_announce_task_t *)calloc(1, sizeof(*t));
        if (t) {
            t->node = svc->node;
            if (node_post(svc->node, announce_task_fn, t) != OK) {
                free(t);
            }
        }
    }
}

int service_set_interceptor(service_t *svc, service_interceptor_fn fn, void *user)
{
    if (!svc) {
        return ERR_INVALID;
    }
    if (svc->node && svc->node->services_lock) {
        ntk_mutex_lock(svc->node->services_lock);
    }
    svc->interceptor = fn;
    svc->interceptor_user = user;
    if (svc->node && svc->node->services_lock) {
        ntk_mutex_unlock(svc->node->services_lock);
    }
    return OK;
}

/* ---- 方法注册 ---- */

/*
 * 解析调用者选择的执行模式。allow_worker 为 0 时只允许 INLINE（流式方法）。
 * 输出 run_in_worker / executor 二选一。
 */
static int service_parse_exec(const service_method_options_t *options,
                              int allow_worker,
                              int *out_worker,
                              const service_executor_t **out_executor)
{
    service_exec_mode_t exec = options ? options->exec : SERVICE_EXEC_INLINE;
    const service_executor_t *executor = options ? options->executor : NULL;

    *out_worker = 0;
    *out_executor = NULL;

    if (exec == SERVICE_EXEC_INLINE) {
        return executor ? ERR_INVALID : OK;
    }
    if (exec == SERVICE_EXEC_WORKER && allow_worker) {
        if (executor) {
            return ERR_INVALID;
        }
        *out_worker = 1;
        return OK;
    }
    if (exec == SERVICE_EXEC_EXECUTOR && allow_worker) {
        if (!executor || !executor->submit) {
            return ERR_INVALID;
        }
        *out_executor = executor;
        return OK;
    }
    return ERR_INVALID;
}

static int service_method_register(service_t *svc,
                                   const char *method,
                                   handler_fn fn,
                                   chunk_handler_fn stream_fn,
                                   void *user,
                                   int owns_user,
                                   int run_in_worker,
                                   const service_executor_t *executor)
{
    zri_method_t *m = NULL;
    zri_svc_task_t *t;

    if (!svc || !method || !method[0] || (!fn && !stream_fn)) {
        return ERR_INVALID;
    }
    m = (zri_method_t *)calloc(1, sizeof(*m));
    if (!m) {
        return ERR_NOMEM;
    }
    zri_copy_str(m->name, sizeof(m->name), method, "m");
    m->fn = fn;
    m->stream_fn = stream_fn;
    m->user = user;
    m->owns_user = owns_user;
    m->run_in_worker = run_in_worker;
    m->executor = executor;
    t = (zri_svc_task_t *)calloc(1, sizeof(*t));
    if (!t) {
        free(m);
        return ERR_NOMEM;
    }
    t->node = svc->node;
    t->svc = svc;
    t->method = m;
    if (ntk_poller_async(svc->node->poller, svc_method_task, t, 1) != NTK_OK) {
        free(t);
        free(m);
        return ERR_IO;
    }
    return OK;
}

int service_add_method(service_t *svc, const char *method, handler_fn fn, void *user)
{
    return service_method_register(svc, method, fn, NULL, user, 0, 0, NULL);
}

int service_add_method_ex(service_t *svc,
                          const char *method,
                          handler_fn fn,
                          void *user,
                          const service_method_options_t *options)
{
    int run_in_worker;
    const service_executor_t *executor;
    int rc = service_parse_exec(options, 1, &run_in_worker, &executor);
    if (rc != OK) {
        return rc;
    }
    return service_method_register(svc, method, fn, NULL, user, 0, run_in_worker, executor);
}

int service_add_method_worker(service_t *svc, const char *method, handler_fn fn, void *user)
{
    return service_method_register(svc, method, fn, NULL, user, 0, 1, NULL);
}

static int service_add_sync_ex(service_t *svc,
                               const char *method,
                               sync_handler_fn fn,
                               void *user,
                               const service_method_options_t *options)
{
    zri_sync_bridge_t *bridge;
    int run_in_worker;
    const service_executor_t *executor;
    int rc;
    if (!svc || !method || !method[0] || !fn) {
        return ERR_INVALID;
    }
    rc = service_parse_exec(options, 1, &run_in_worker, &executor);
    if (rc != OK) {
        return rc;
    }
    bridge = (zri_sync_bridge_t *)calloc(1, sizeof(*bridge));
    if (!bridge) {
        return ERR_NOMEM;
    }
    bridge->fn = fn;
    bridge->user = user;
    rc = service_method_register(
        svc, method, sync_method_bridge, NULL, bridge, 1, run_in_worker, executor);
    if (rc != OK) {
        free(bridge);
    }
    return rc;
}

int service_add_sync_method(service_t *svc, const char *method, sync_handler_fn fn, void *user)
{
    return service_add_sync_ex(svc, method, fn, user, NULL);
}

int service_add_sync_method_ex(service_t *svc,
                               const char *method,
                               sync_handler_fn fn,
                               void *user,
                               const service_method_options_t *options)
{
    return service_add_sync_ex(svc, method, fn, user, options);
}

int service_add_sync_method_worker(service_t *svc,
                                   const char *method,
                                   sync_handler_fn fn,
                                   void *user)
{
    service_method_options_t options = SERVICE_METHOD_OPTIONS_INIT;
    options.exec = SERVICE_EXEC_WORKER;
    return service_add_sync_ex(svc, method, fn, user, &options);
}

int service_add_stream_method(service_t *svc, const char *method, chunk_handler_fn fn, void *user)
{
    return service_method_register(svc, method, NULL, fn, user, 0, 0, NULL);
}

int service_add_stream_method_ex(service_t *svc,
                                 const char *method,
                                 chunk_handler_fn fn,
                                 void *user,
                                 const service_method_options_t *options)
{
    int run_in_worker;
    const service_executor_t *executor;
    int rc = service_parse_exec(options, 1, &run_in_worker, &executor);
    if (rc != OK) {
        return rc;
    }
    return service_method_register(svc, method, NULL, fn, user, 0, run_in_worker, executor);
}
