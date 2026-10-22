/* 服务端：方法注册与请求分发（回复见 nb_reply.c）。 */
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
typedef struct nb_sync_bridge {
    sync_handler_fn fn;
    void *user;
} nb_sync_bridge_t;

static void nb_sync_method_bridge(nb_call_t *call, const nb_request_t *req, void *user)
{
    nb_sync_bridge_t *bridge = (nb_sync_bridge_t *)user;
    nb_message_view_t view;
    nb_payload_t pl;
    int status;
    nb_message_view_init(&view);
    status = bridge->fn(req, &view, bridge->user);
    nb_message_view_to_payload(&view, &pl);
    nb_reply(call, status, &pl);
    nb_message_view_release(&view);
}

typedef struct nb_svc_task {
    nb_node_t *node;
    nb_service_t *svc;
    nb_method_t *method;
} nb_svc_task_t;

typedef struct nb_announce_task {
    nb_node_t *node;
} nb_announce_task_t;

static void nb_announce_task_fn(void *user)
{
    nb_announce_task_t *t = (nb_announce_task_t *)user;
    if (t) {
        if (t->node && t->node->registry_backend && t->node->registry_backend->ops->announce) {
            t->node->registry_backend->ops->announce(t->node, 0);
        }
        free(t);
    }
}

static void nb_svc_link_task(void *user)
{
    nb_svc_task_t *t = (nb_svc_task_t *)user;
    ntk_mutex_lock(t->node->services_lock);
    t->svc->next = t->node->services;
    t->node->services = t->svc;
    ntk_mutex_unlock(t->node->services_lock);
    if (t->node->registry_backend && t->node->registry_backend->ops->announce) {
        t->node->registry_backend->ops->announce(t->node, 0);
    }
    free(t);
}

static void nb_svc_method_task(void *user)
{
    nb_svc_task_t *t = (nb_svc_task_t *)user;
    ntk_mutex_lock(t->node->services_lock);
    t->method->next = t->svc->methods;
    t->svc->methods = t->method;
    ntk_mutex_unlock(t->node->services_lock);
    free(t);
}

/*
 * 阻塞 handler 的线程池任务：请求字段全部深拷贝。worker 里构造 call 并执行
 * 用户 handler；nb_reply() 会自动把响应回投到 origin poller 发送。
 */
#define HANDLER_INLINE_PAYLOAD 128

typedef struct nb_handler_task {
    nb_node_t *node;
    nb_transport_t *transport;
    nb_peer_token_t token;
    ntk_poller *poller;
    char peer_ip[NB_ENDPOINT_MAX];
    uint16_t peer_port;
    uint32_t msg_id;
    int one_way;
    handler_fn fn;
    void *user;
    service_interceptor_fn interceptor;
    void *interceptor_user;
    char service[NB_SERVICE_NAME_MAX];
    char method[NB_METHOD_NAME_MAX];
    uint8_t *payload;
    uint8_t inline_payload[HANDLER_INLINE_PAYLOAD]; /* 小请求免一次 malloc */
    size_t payload_len;
    uint8_t encoding;
    uint64_t deadline_ms; /* 请求 deadline（绝对单调 ms，0 = 无） */
    nb_metadata_t *metadata; /* 请求 metadata 解码副本，按需分配；无则为 NULL */
    int external;         /* 1 = 自定义 executor 任务 */
} nb_handler_task_t;

static void nb_handler_inflight_release(nb_node_t *node);
static void nb_handler_external_release(nb_node_t *node);

static void nb_handler_task_release(nb_handler_task_t *t)
{
    if (t->external) {
        nb_handler_inflight_release(t->node);
        nb_handler_external_release(t->node);
    } else {
        nb_handler_inflight_release(t->node);
    }
}

static void nb_handler_task_destroy(nb_handler_task_t *t)
{
    if (!t) {
        return;
    }
    if (t->payload && t->payload != t->inline_payload) {
        free(t->payload);
    }
    free(t->metadata);
    if (t->node && t->node->handler_task_pool.lock) {
        nb_obj_pool_free(&t->node->handler_task_pool, t);
    } else {
        free(t);
    }
}

/* worker 路径的 call：优先取节点复用池，抢不到锁/池空则回退 malloc。 */
nb_call_t *nb_worker_call_alloc(nb_node_t *node)
{
    nb_call_t *call;
    if (!node) {
        return NULL;
    }
    call = (nb_call_t *)nb_obj_pool_alloc(&node->worker_call_pool, sizeof(*call));
    if (!call) {
        return NULL;
    }
    memset(call, 0, sizeof(*call));
    call->from_worker_pool = 1;
    return call;
}

void nb_worker_call_free(nb_node_t *node, nb_call_t *call)
{
    if (!call) {
        return;
    }
    if (call->from_worker_pool) {
        if (node && node->worker_call_pool.lock) {
            nb_obj_pool_free(&node->worker_call_pool, call);
        } else {
            free(call);
        }
        return;
    }
    nb_mem_free(call);
}

static void nb_handler_task_run(void *user)
{
    nb_handler_task_t *t = (nb_handler_task_t *)user;
    nb_call_t *call;
    nb_request_t req;
    int one_way = t->one_way;

    /* worker 线程没有 current poller，nb_mem_alloc 会回退到 malloc。 */
    call = nb_worker_call_alloc(t->node);
    if (!call) {
        nb_handler_task_release(t);
        nb_handler_task_destroy(t);
        return;
    }
    /* nb_worker_call_alloc 已清零并设置 from_worker_pool，这里不能再 memset。 */
    call->node = t->node;
    call->transport = t->transport;
    call->poller = t->poller;
    call->token = t->token;
    nb_copy_str(call->peer_ip, sizeof(call->peer_ip), t->peer_ip, "0.0.0.0");
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
        int canceled = nb_node_is_canceled(t->node, t->msg_id, t->peer_ip, t->peer_port);
        int expired = t->deadline_ms != 0 && nb_now_ms() >= t->deadline_ms;
        if (canceled || expired) {
            if (!one_way) {
                nb_reply(call, canceled ? NB_ERR_NOTFOUND : NB_ERR_TIMEOUT, NULL);
            } else {
                nb_worker_call_free(t->node, call);
            }
            nb_handler_task_release(t);
            nb_handler_task_destroy(t);
            return;
        }
    }
    if (t->interceptor) {
        int st = t->interceptor(call, &req, t->interceptor_user);
        if (st != NB_OK) {
            if (!one_way) {
                nb_reply(call, st, NULL);
            } else {
                nb_worker_call_free(t->node, call);
            }
            nb_handler_task_release(t);
            nb_handler_task_destroy(t);
            return;
        }
    }
    nb_log_debug("worker request %s.%s id=%llu len=%zu",
                  t->service,
                  t->method,
                  (unsigned long long)t->msg_id,
                  t->payload_len);

    t->fn(call, &req, t->user);
    nb_handler_task_release(t);
    /* one_way 没有 nb_reply，call 在 handler 返回后释放；请求载荷也可释放。 */
    if (one_way) {
        nb_worker_call_free(t->node, call);
    }
    nb_handler_task_destroy(t);
}

/*
 * worker 未完成任务（排队 + 执行中）限流。0 表示不限制；超限返回 NB_ERR_AGAIN，
 * 由调用方在 poller 上直接回错误状态，避免无界排队。
 */
static int nb_handler_inflight_acquire(nb_node_t *node)
{
    int ok;
    if (node->handler_max_concurrency == 0 || !node->handler_limit_mutex) {
        return NB_OK;
    }
    ntk_mutex_lock(node->handler_limit_mutex);
    ok = node->handler_inflight < node->handler_max_concurrency;
    if (ok) {
        node->handler_inflight++;
    }
    ntk_mutex_unlock(node->handler_limit_mutex);
    return ok ? NB_OK : NB_ERR_AGAIN;
}

static void nb_handler_inflight_release(nb_node_t *node)
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

static void nb_handler_external_acquire(nb_node_t *node)
{
    if (!node->handler_limit_mutex) {
        return;
    }
    ntk_mutex_lock(node->handler_limit_mutex);
    node->external_inflight++;
    ntk_mutex_unlock(node->handler_limit_mutex);
}

static void nb_handler_external_release(nb_node_t *node)
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

void nb_node_cancel_record(nb_node_t *node,
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
    now = nb_now_ms();
    ntk_mutex_lock(node->handler_limit_mutex);
    for (i = 0; i < CANCEL_SLOTS; i++) {
        nb_cancel_slot_t *s = &node->cancels[i];
        if (s->used && s->expires_ms <= now) {
            s->used = 0;
            if (node->cancel_count > 0) {
                nb_atomic_sub_u32(&node->cancel_count, 1u);
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
        nb_cancel_slot_t *s = &node->cancels[free_slot];
        s->used = 1;
        s->request_id = request_id;
        nb_copy_str(s->peer_ip, sizeof(s->peer_ip), peer_ip, "0.0.0.0");
        s->peer_port = peer_port;
        s->expires_ms = now + CANCEL_TTL_MS;
        nb_atomic_add_u32(&node->cancel_count, 1u);
    }
    ntk_mutex_unlock(node->handler_limit_mutex);
}

int nb_node_is_canceled(const nb_node_t *node,
                         uint32_t request_id,
                         const char *peer_ip,
                         uint16_t peer_port)
{
    int found = 0;
    int i;
    uint64_t now;
    if (!node || !node->handler_limit_mutex ||
        nb_atomic_load_u32(&node->cancel_count) == 0) {
        return 0;
    }
    now = nb_now_ms();
    ntk_mutex_lock(node->handler_limit_mutex);
    for (i = 0; i < CANCEL_SLOTS; i++) {
        const nb_cancel_slot_t *s = &node->cancels[i];
        if (s->used && s->expires_ms > now && s->request_id == request_id &&
            s->peer_port == peer_port && strcmp(s->peer_ip, peer_ip ? peer_ip : "") == 0) {
            found = 1;
            break;
        }
    }
    ntk_mutex_unlock(node->handler_limit_mutex);
    return found;
}

void nb_node_expire_cancels(nb_node_t *node, uint64_t now_ms)
{
    int i;
    if (!node || !node->handler_limit_mutex) {
        return;
    }
    ntk_mutex_lock(node->handler_limit_mutex);
    for (i = 0; i < CANCEL_SLOTS; i++) {
        nb_cancel_slot_t *s = &node->cancels[i];
        if (s->used && s->expires_ms <= now_ms) {
            s->used = 0;
            if (node->cancel_count > 0) {
                nb_atomic_sub_u32(&node->cancel_count, 1u);
            }
        }
    }
    ntk_mutex_unlock(node->handler_limit_mutex);
}

/* 由客户端 timeout_ms 换算成绝对 deadline（接收时刻起算）。 */
static uint64_t nb_deadline_from_timeout(uint32_t timeout_ms)
{
    return timeout_ms ? nb_now_ms() + timeout_ms : 0;
}

/* 把已解析好的请求深拷贝成可跨线程执行的任务。 */
static int nb_handler_task_create(nb_node_t *node,
                               nb_transport_t *transport,
                               const nb_message_t *msg,
                               const char *service,
                               const char *method,
                               handler_fn fn,
                               void *user,
                               service_interceptor_fn interceptor,
                               void *interceptor_user,
                               int external,
                               nb_handler_task_t **out)
{
    nb_handler_task_t *t;
    size_t payload_len = msg->payload.len;

    *out = NULL;
    if (payload_len > 0 && !msg->payload.data) {
        return NB_ERR_INVALID;
    }
    t = (nb_handler_task_t *)nb_obj_pool_alloc(&node->handler_task_pool, sizeof(*t));
    if (!t) {
        return NB_ERR_NOMEM;
    }
    memset(t, 0, sizeof(*t));
    t->node = node;
    t->transport = transport;
    t->token = msg->peer_token;
    t->poller = ntk_poller_current();
    nb_copy_str(t->peer_ip, sizeof(t->peer_ip), msg->peer_ip, "0.0.0.0");
    t->peer_port = msg->peer_port;
    t->msg_id = msg->request_id;
    t->one_way = msg->kind == KIND_EVENT;
    t->fn = fn;
    t->user = user;
    t->interceptor = interceptor;
    t->interceptor_user = interceptor_user;
    t->external = external;
    nb_copy_str(t->service, sizeof(t->service), service, "");
    nb_copy_str(t->method, sizeof(t->method), method, "");
    t->encoding = msg->payload.encoding;
    t->deadline_ms = nb_deadline_from_timeout(msg->timeout_ms);
    if (msg->metadata_len > 0 && msg->metadata) {
        nb_metadata_t *md = (nb_metadata_t *)malloc(sizeof(*md));
        if (md) {
            if (nb_metadata_decode(md, msg->metadata, msg->metadata_len) == NB_OK && md->count > 0) {
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
            nb_handler_task_destroy(t);
            return NB_ERR_NOMEM;
        }
        memcpy(t->payload, msg->payload.data, payload_len);
    }
    *out = t;
    return NB_OK;
}

/* 投递到节点内置 handler 线程池。 */
static int nb_service_dispatch_worker(nb_node_t *node,
                                   nb_transport_t *transport,
                                   const nb_message_t *msg,
                                   const char *service,
                                   const char *method,
                                   handler_fn fn,
                                   void *user,
                                   service_interceptor_fn interceptor,
                                   void *interceptor_user)
{
    nb_handler_task_t *t = NULL;
    int rc;

    rc = nb_handler_inflight_acquire(node);
    if (rc != NB_OK) {
        return rc;
    }
    rc = nb_handler_task_create(
        node, transport, msg, service, method, fn, user, interceptor, interceptor_user, 0, &t);
    if (rc != NB_OK) {
        nb_handler_inflight_release(node);
        return rc;
    }
    if (ntk_thread_pool_async(node->handler_pool, nb_handler_task_run, t, 1) != NTK_OK) {
        nb_handler_inflight_release(node);
        nb_handler_task_destroy(t);
        return NB_ERR_IO;
    }
    return NB_OK;
}

/* 投递到调用者提供的 executor（借用语义）。 */
static int nb_service_dispatch_external(nb_node_t *node,
                                     nb_transport_t *transport,
                                     const nb_message_t *msg,
                                     const char *service,
                                     const char *method,
                                     handler_fn fn,
                                     void *user,
                                     const nb_service_executor_t *executor,
                                     service_interceptor_fn interceptor,
                                     void *interceptor_user)
{
    nb_handler_task_t *t = NULL;
    int rc;

    /* 自定义 executor 与内置 worker 共用同一未完成上限，避免执行器侧无界排队。 */
    rc = nb_handler_inflight_acquire(node);
    if (rc != NB_OK) {
        return rc;
    }
    rc = nb_handler_task_create(
        node, transport, msg, service, method, fn, user, interceptor, interceptor_user, 1, &t);
    if (rc != NB_OK) {
        nb_handler_inflight_release(node);
        return rc;
    }
    nb_handler_external_acquire(node);
    rc = executor->submit(executor->ctx, nb_handler_task_run, t);
    if (rc != NB_OK) {
        nb_handler_external_release(node);
        nb_handler_inflight_release(node);
        nb_handler_task_destroy(t);
        return rc;
    }
    return NB_OK;
}

void nb_service_handle_request(nb_node_t *node, nb_transport_t *transport, const nb_message_t *msg)
{
    char tmp[MAX_ROUTE];
    char *dot;
    nb_service_t *svc;
    nb_method_t *m = NULL;
    handler_fn fn = NULL;
    void *user = NULL;
    service_interceptor_fn interceptor = NULL;
    void *interceptor_user = NULL;
    int dedup_active = 0;

    if (!node || !msg) {
        return;
    }
    nb_copy_str(tmp, sizeof(tmp), msg->route, "");
    dot = strchr(tmp, '.');
    if (!dot) {
        nb_service_send_status(transport, msg, NB_ERR_NOTFOUND);
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
        nb_service_send_status(transport, msg, NB_ERR_NOTFOUND);
        return;
    }
    /*
     * UDP unary 去重：命中已完成请求直接重放缓存响应；命中在途请求丢弃重复，
     * 避免非幂等 handler 被重传执行多次。TCP 可靠，不参与。
     */
    if (transport && transport->kind == NB_TRANSPORT_UDP && msg->kind == KIND_REQUEST &&
        msg->request_id != 0) {
        int dstate = 0;
        (void)nb_dedup_begin(&node->dedup, transport, msg, &dstate);
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
        int rc = nb_service_dispatch_external(node,
                                           transport,
                                           msg,
                                           tmp,
                                           dot + 1,
                                           fn,
                                           user,
                                           m->executor,
                                           interceptor,
                                           interceptor_user);
        if (rc == NB_OK) {
            return;
        }
        nb_log_error("executor dispatch failed: service=%s method=%s rc=%d", tmp, dot + 1, rc);
        if (dedup_active) {
            nb_dedup_abort(&node->dedup, msg);
        }
        nb_service_send_status(transport, msg, rc);
        return;
    }
    if (m->run_in_worker) {
        if (node->handler_pool) {
            int rc = nb_service_dispatch_worker(node,
                                             transport,
                                             msg,
                                             tmp,
                                             dot + 1,
                                             fn,
                                             user,
                                             interceptor,
                                             interceptor_user);
            if (rc == NB_OK) {
                return;
            }
            if (rc == NB_ERR_AGAIN) {
                nb_log_warn("worker overloaded: service=%s method=%s", tmp, dot + 1);
            } else {
                nb_log_error(
                    "worker dispatch failed: service=%s method=%s rc=%d", tmp, dot + 1, rc);
            }
            if (dedup_active) {
                nb_dedup_abort(&node->dedup, msg);
            }
            nb_service_send_status(transport, msg, rc);
            return;
        }
        if (nb_atomic_exchange_u32((uint32_t *)&node->worker_fallback_warned, 1u) == 0) {
            nb_log_warn(
                "handler_threads=0: worker method %s.%s runs inline on poller", tmp, dot + 1);
        }
    }
    nb_log_debug("request %s.%s id=%llu len=%zu peer=%s:%u",
                  tmp,
                  dot + 1,
                  (unsigned long long)msg->request_id,
                  msg->payload.len,
                  msg->peer_ip ? msg->peer_ip : "?",
                  (unsigned)msg->peer_port);
    {
        nb_call_t *call = (nb_call_t *)nb_mem_alloc(sizeof(*call));
        nb_request_t req;
        nb_metadata_t request_metadata;
        int has_metadata = 0;
        if (!call) {
            if (dedup_active) {
                nb_dedup_abort(&node->dedup, msg);
            }
            nb_service_send_status(transport, msg, NB_ERR_NOMEM);
            return;
        }
        memset(call, 0, sizeof(*call));
        call->node = node;
        call->transport = transport;
        call->poller = ntk_poller_current();
        call->token = msg->peer_token;
        nb_copy_str(call->peer_ip, sizeof(call->peer_ip), msg->peer_ip, "0.0.0.0");
        call->peer_port = msg->peer_port;
        call->msg_id = msg->request_id;
        call->one_way = msg->kind == KIND_EVENT;
        call->deadline_ms = nb_deadline_from_timeout(msg->timeout_ms);
        req.service = tmp;
        req.method = dot + 1;
        req.payload = msg->payload;
        req.peer_ip = call->peer_ip;
        req.peer_port = msg->peer_port;
        req.deadline_ms = call->deadline_ms;
        if (msg->metadata_len > 0 && msg->metadata &&
            nb_metadata_decode(&request_metadata, msg->metadata, msg->metadata_len) == NB_OK &&
            request_metadata.count > 0) {
            has_metadata = 1;
        }
        req.metadata = has_metadata ? &request_metadata : NULL;
        {
            int one_way = call->one_way;
            int st = interceptor ? interceptor(call, &req, interceptor_user) : NB_OK;
            if (st == NB_OK) {
                fn(call, &req, user);
            } else {
                nb_reply(call, st, NULL);
            }
            if (one_way) {
                nb_mem_free(call);
            }
        }
    }
}

/*
 * 客户端取消：按 request_id + 对端登记。worker 任务启动前会查表跳过；
 * 已运行的 handler 可通过 nb_call_canceled() 观测（无法抢占）。
 */
void nb_service_handle_cancel(nb_node_t *node, nb_transport_t *transport, const nb_message_t *msg)
{
    (void)transport;
    if (!node || !msg || msg->kind != KIND_CANCEL) {
        return;
    }
    nb_node_cancel_record(node, msg->request_id, msg->peer_ip, msg->peer_port);
}

uint64_t nb_call_deadline_ms(const nb_call_t *call)
{
    return call ? call->deadline_ms : 0;
}

int nb_call_canceled(const nb_call_t *call)
{
    if (!call) {
        return 0;
    }
    return nb_node_is_canceled(call->node, call->msg_id, call->peer_ip, call->peer_port);
}

static nb_recv_stream_t *nb_find_recv_stream(nb_node_t *node, const nb_message_t *msg)
{
    int i;
    for (i = 0; i < MAX_RECV_STREAMS; i++) {
        nb_recv_stream_t *stream = &node->recv_streams[i];
        if (stream->used && stream->request_id == msg->request_id &&
            stream->peer_port == msg->peer_port &&
            strcmp(stream->peer_ip, msg->peer_ip ? msg->peer_ip : "") == 0) {
            return stream;
        }
    }
    return NULL;
}

static nb_recv_stream_t *nb_alloc_recv_stream(nb_node_t *node,
                                            nb_transport_t *transport,
                                            const nb_message_t *msg)
{
    int i;
    int used = 0;
    for (i = 0; i < MAX_RECV_STREAMS; i++) {
        if (node->recv_streams[i].used) {
            used++;
        }
    }
    if (node->max_recv_streams != 0 && (uint32_t)used >= node->max_recv_streams) {
        NB_METRIC_INC(node->metrics.streams_rejected);
        return NULL;
    }
    for (i = 0; i < MAX_RECV_STREAMS; i++) {
        nb_recv_stream_t *stream = &node->recv_streams[i];
        if (!stream->used) {
            nb_call_t *call = (nb_call_t *)nb_mem_alloc(sizeof(*call));
            if (!call) {
                return NULL;
            }
            memset(call, 0, sizeof(*call));
            stream->used = 1;
            stream->request_id = msg->request_id;
            nb_copy_str(stream->peer_ip, sizeof(stream->peer_ip), msg->peer_ip, "0.0.0.0");
            stream->peer_port = msg->peer_port;
            stream->call = call;
            stream->last_ms = nb_now_ms();
            stream->bytes = 0;
            call->node = node;
            call->transport = transport;
            call->poller = ntk_poller_current();
            call->token = msg->peer_token;
            nb_copy_str(call->peer_ip, sizeof(call->peer_ip), msg->peer_ip, "0.0.0.0");
            call->peer_port = msg->peer_port;
            call->msg_id = msg->request_id;
            call->stream_owned = 1;
            call->deadline_ms = nb_deadline_from_timeout(msg->timeout_ms);
            return stream;
        }
    }
    return NULL;
}

/* ---- 流式 chunk 的 worker/executor 派发（每个 stream 串行，保证顺序） ---- */

typedef struct nb_stream_task {
    nb_recv_stream_t *state;
    nb_call_t *call;
    uint8_t *data;
    size_t len;
    uint8_t encoding;
    unsigned flags;
    uint64_t offset;
    const nb_metadata_t *metadata; /* 借用 stream 状态里的副本；仅本次任务内有效 */
    nb_metadata_t chunk_metadata;  /* 本 chunk 自带 metadata 的解码副本 */
    int has_chunk_metadata;
    char message[NB_STATUS_MSG_MAX]; /* 本 chunk 的状态消息 */
    uint16_t message_len;
    uint64_t deadline_ms;
    service_interceptor_fn interceptor;
    void *interceptor_user;
    chunk_handler_fn fn;
    void *user;
    int first;
    int rejected;
    struct nb_stream_task *next;
} nb_stream_task_t;

static void nb_stream_task_run(void *user);
static void nb_stream_task_complete(void *user);

static void nb_stream_task_free(nb_stream_task_t *t)
{
    if (!t) {
        return;
    }
    free(t->data);
    free(t);
}

static void nb_stream_clear_pending(nb_recv_stream_t *state)
{
    nb_stream_task_t *t = state->pending_head;
    while (t) {
        nb_stream_task_t *next = t->next;
        nb_stream_task_free(t);
        t = next;
    }
    state->pending_head = NULL;
    state->pending_tail = NULL;
}

static void nb_stream_close(nb_recv_stream_t *state)
{
    nb_call_t *call = state->call;
    nb_stream_clear_pending(state);
    free(state->metadata);
    memset(state, 0, sizeof(*state));
    if (call) {
        nb_mem_free(call);
    }
}

static int nb_stream_submit(nb_recv_stream_t *state, nb_stream_task_t *task)
{
    nb_method_t *m = state->method;
    nb_node_t *node = state->call ? state->call->node : NULL;
    if (!m) {
        nb_stream_task_free(task);
        return NB_ERR_STATE;
    }
    if (m->executor) {
        if (m->executor->submit(m->executor->ctx, nb_stream_task_run, task) != NB_OK) {
            nb_stream_task_free(task);
            return NB_ERR_IO;
        }
        return NB_OK;
    }
    if (node && node->handler_pool) {
        if (ntk_thread_pool_async(node->handler_pool, nb_stream_task_run, task, 1) != NTK_OK) {
            nb_stream_task_free(task);
            return NB_ERR_IO;
        }
        return NB_OK;
    }
    nb_stream_task_free(task);
    return NB_ERR_STATE;
}

static void nb_stream_dispatch_next(nb_recv_stream_t *state)
{
    nb_stream_task_t *task;
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
    if (nb_stream_submit(state, task) != NB_OK) {
        state->in_flight = 0;
        state->closing = 1;
    }
}

static void nb_stream_task_run(void *user)
{
    nb_stream_task_t *task = (nb_stream_task_t *)user;
    nb_recv_stream_t *state = task->state;
    nb_call_t *call = task->call;
    nb_request_t req;
    nb_chunk_t chunk;
    int rejected = 0;

    if (!state || !call) {
        nb_stream_task_free(task);
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
    chunk.metadata = task->has_chunk_metadata ? &task->chunk_metadata : NULL;
    chunk.message = task->message_len ? task->message : NULL;
    if (task->first && task->interceptor) {
        int st = task->interceptor(call, &req, task->interceptor_user);
        if (st != NB_OK) {
            nb_reply_stream(call, st, NULL);
            rejected = 1;
        }
    }
    if (!rejected) {
        task->fn(call, &req, &chunk, task->user);
    }
    task->rejected = rejected;
    if (ntk_poller_async(call->poller, nb_stream_task_complete, task, 1) != NTK_OK) {
        nb_stream_task_free(task);
    }
}

static void nb_stream_task_complete(void *user)
{
    nb_stream_task_t *task = (nb_stream_task_t *)user;
    nb_recv_stream_t *state = task->state;
    int rejected = task->rejected;
    nb_stream_task_free(task);
    if (!state || !state->used) {
        return;
    }
    state->in_flight = 0;
    state->last_ms = nb_now_ms();
    if (rejected) {
        state->closing = 1;
        nb_stream_clear_pending(state);
    }
    if (state->closing && !state->pending_head) {
        nb_stream_close(state);
        return;
    }
    nb_stream_dispatch_next(state);
}

void nb_service_handle_stream_chunk(nb_node_t *node,
                                     nb_transport_t *transport,
                                     const nb_message_t *msg)
{
    char tmp[MAX_ROUTE];
    char *dot;
    nb_service_t *svc = NULL;
    nb_method_t *method = NULL;
    nb_recv_stream_t *state;
    nb_request_t request;
    nb_chunk_t chunk;
    nb_metadata_t chunk_metadata;
    int has_chunk_metadata = 0;
    char chunk_message[NB_STATUS_MSG_MAX];
    uint16_t chunk_message_len = 0;
    const nb_stream_meta_t *stream_meta;
    int use_worker;
    service_interceptor_fn interceptor = NULL;
    void *interceptor_user = NULL;

    if (!node || !msg || !msg->has_stream) {
        return;
    }
    stream_meta = &msg->stream;
    state = nb_find_recv_stream(node, msg);

    if (stream_meta->flags & NB_STREAM_CANCEL) {
        if (state) {
            if ((state->run_in_worker || state->executor) &&
                (state->in_flight || state->pending_head)) {
                state->closing = 1;
                state->canceled = 1;
                nb_stream_clear_pending(state);
                if (!state->in_flight) {
                    nb_stream_close(state);
                }
            } else {
                nb_stream_close(state);
            }
        }
        return;
    }

    if (state) {
        method = state->method;
    } else {
        if (!(stream_meta->flags & NB_STREAM_FIRST)) {
            return; /* 没有 FIRST 也没有已有状态：协议错误 */
        }
        nb_copy_str(tmp, sizeof(tmp), msg->route, "");
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
        state = nb_alloc_recv_stream(node, transport, msg);
        if (state) {
            state->method = method;
            state->run_in_worker = method->run_in_worker;
            state->executor = method->executor;
            state->interceptor = interceptor;
            state->interceptor_user = interceptor_user;
            state->deadline_ms = state->call ? state->call->deadline_ms : 0;
            if (msg->metadata_len > 0 && msg->metadata) {
                nb_metadata_t *md = (nb_metadata_t *)calloc(1, sizeof(*md));
                if (md) {
                    if (nb_metadata_decode(md, msg->metadata, msg->metadata_len) == NB_OK &&
                        md->count > 0) {
                        state->metadata = md;
                    } else {
                        free(md);
                    }
                }
            }
            nb_copy_str(state->service, sizeof(state->service), tmp, "");
            nb_copy_str(state->method_name, sizeof(state->method_name), dot + 1, "");
        }
    }
    if (!state || !state->call || !method) {
        return;
    }
    use_worker = (state->run_in_worker && node->handler_pool) || state->executor;
    if (use_worker) {
        int first = !state->intercepted;
        nb_stream_task_t *task = (nb_stream_task_t *)calloc(1, sizeof(*task));
        if (!task) {
            state->closing = 1;
            nb_stream_close(state);
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
                nb_stream_close(state);
                return;
            }
            memcpy(task->data, msg->payload.data, task->len);
        }
        if (state->metadata) {
            task->metadata = state->metadata;
        }
        if (msg->metadata_len > 0 && msg->metadata &&
            nb_metadata_decode(&task->chunk_metadata, msg->metadata, msg->metadata_len) == NB_OK &&
            task->chunk_metadata.count > 0) {
            task->has_chunk_metadata = 1;
        }
        if (msg->status_msg_len > 0 && msg->status_msg) {
            size_t message_len = msg->status_msg_len;
            if (message_len >= sizeof(task->message)) {
                message_len = sizeof(task->message) - 1;
            }
            memcpy(task->message, msg->status_msg, message_len);
            task->message[message_len] = '\0';
            task->message_len = (uint16_t)message_len;
        }
        state->intercepted = 1;
        state->last_ms = nb_now_ms();
        state->bytes += msg->payload.len;
        if (state->pending_tail) {
            state->pending_tail->next = task;
        } else {
            state->pending_head = task;
        }
        state->pending_tail = task;
        if (stream_meta->flags & NB_STREAM_LAST) {
            state->closing = 1;
        }
        nb_stream_dispatch_next(state);
        if (state->closing && !state->in_flight && !state->pending_head) {
            nb_stream_close(state);
        }
        return;
    }
    if (state->run_in_worker && !node->handler_pool &&
        nb_atomic_exchange_u32((uint32_t *)&node->worker_fallback_warned, 1u) == 0) {
        nb_log_warn("handler_threads=0: worker stream %s.%s runs inline on poller",
                     state->service,
                     state->method_name);
    }
    state->last_ms = nb_now_ms();
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
    if (msg->metadata_len > 0 && msg->metadata &&
        nb_metadata_decode(&chunk_metadata, msg->metadata, msg->metadata_len) == NB_OK &&
        chunk_metadata.count > 0) {
        has_chunk_metadata = 1;
    }
    chunk.metadata = has_chunk_metadata ? &chunk_metadata : NULL;
    if (msg->status_msg_len > 0 && msg->status_msg) {
        size_t message_len = msg->status_msg_len;
        if (message_len >= sizeof(chunk_message)) {
            message_len = sizeof(chunk_message) - 1;
        }
        memcpy(chunk_message, msg->status_msg, message_len);
        chunk_message[message_len] = '\0';
        chunk_message_len = (uint16_t)message_len;
    }
    chunk.message = chunk_message_len ? chunk_message : NULL;

    if (!state->intercepted && state->interceptor) {
        int st = state->interceptor(state->call, &request, state->interceptor_user);
        if (st != NB_OK) {
            state->intercepted = 1;
            nb_reply_stream(state->call, st, NULL);
            nb_stream_close(state);
            return;
        }
    }
    state->intercepted = 1;
    method->stream_fn(state->call, &request, &chunk, method->user);

    if (stream_meta->flags & NB_STREAM_LAST) {
        nb_stream_close(state);
    }
}

void nb_service_expire_streams(nb_node_t *node, uint64_t now_ms)
{
    int i;
    if (!node) {
        return;
    }
    for (i = 0; i < MAX_RECV_STREAMS; i++) {
        nb_recv_stream_t *stream = &node->recv_streams[i];
        if (stream->used && now_ms - stream->last_ms > STREAM_IDLE_TIMEOUT_MS) {
            if ((stream->run_in_worker || stream->executor) &&
                (stream->in_flight || stream->pending_head)) {
                stream->closing = 1;
                nb_stream_clear_pending(stream);
                if (!stream->in_flight) {
                    nb_stream_close(stream);
                }
            } else {
                nb_stream_close(stream);
            }
        }
    }
}

/* ---- 服务生命周期 ---- */

int nb_service_create(nb_node_t *node, const char *service_name, nb_service_t **out)
{
    nb_service_t *svc;
    nb_svc_task_t *t;

    if (!node || !service_name || !service_name[0] || !out) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    svc = (nb_service_t *)calloc(1, sizeof(*svc));
    if (!svc) {
        return NB_ERR_NOMEM;
    }
    svc->node = node;
    nb_copy_str(svc->name, sizeof(svc->name), service_name, "svc");
    t = (nb_svc_task_t *)calloc(1, sizeof(*t));
    if (!t) {
        free(svc);
        return NB_ERR_NOMEM;
    }
    t->node = node;
    t->svc = svc;
    if (ntk_poller_async(node->poller, nb_svc_link_task, t, 1) != NTK_OK) {
        free(t);
        free(svc);
        return NB_ERR_IO;
    }
    *out = svc;
    return NB_OK;
}

void nb_service_destroy(nb_service_t *svc)
{
    /*
     * 运行期摘除：置 removed 后不再匹配新请求（查找路径跳过）。方法与服务
     * 内存保留到 nb_node_destroy 统一释放，避免在途 handler 仍持有 method 指针
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
        nb_announce_task_t *t = (nb_announce_task_t *)calloc(1, sizeof(*t));
        if (t) {
            t->node = svc->node;
            if (nb_node_post(svc->node, nb_announce_task_fn, t) != NB_OK) {
                free(t);
            }
        }
    }
}

int nb_service_set_interceptor(nb_service_t *svc, service_interceptor_fn fn, void *user)
{
    if (!svc) {
        return NB_ERR_INVALID;
    }
    if (svc->node && svc->node->services_lock) {
        ntk_mutex_lock(svc->node->services_lock);
    }
    svc->interceptor = fn;
    svc->interceptor_user = user;
    if (svc->node && svc->node->services_lock) {
        ntk_mutex_unlock(svc->node->services_lock);
    }
    return NB_OK;
}

/* ---- 方法注册 ---- */

/*
 * 解析调用者选择的执行模式。allow_worker 为 0 时只允许 INLINE（流式方法）。
 * 输出 run_in_worker / executor 二选一。
 */
static int nb_service_parse_exec(const nb_service_method_options_t *options,
                              int allow_worker,
                              int *out_worker,
                              const nb_service_executor_t **out_executor)
{
    nb_service_exec_mode_t exec = options ? options->exec : NB_SERVICE_EXEC_INLINE;
    const nb_service_executor_t *executor = options ? options->executor : NULL;

    *out_worker = 0;
    *out_executor = NULL;

    if (exec == NB_SERVICE_EXEC_INLINE) {
        return executor ? NB_ERR_INVALID : NB_OK;
    }
    if (exec == NB_SERVICE_EXEC_WORKER && allow_worker) {
        if (executor) {
            return NB_ERR_INVALID;
        }
        *out_worker = 1;
        return NB_OK;
    }
    if (exec == NB_SERVICE_EXEC_EXECUTOR && allow_worker) {
        if (!executor || !executor->submit) {
            return NB_ERR_INVALID;
        }
        *out_executor = executor;
        return NB_OK;
    }
    return NB_ERR_INVALID;
}

static int nb_service_method_register(nb_service_t *svc,
                                   const char *method,
                                   handler_fn fn,
                                   chunk_handler_fn stream_fn,
                                   void *user,
                                   int owns_user,
                                   int run_in_worker,
                                   const nb_service_executor_t *executor)
{
    nb_method_t *m = NULL;
    nb_svc_task_t *t;

    if (!svc || !method || !method[0] || (!fn && !stream_fn)) {
        return NB_ERR_INVALID;
    }
    m = (nb_method_t *)calloc(1, sizeof(*m));
    if (!m) {
        return NB_ERR_NOMEM;
    }
    nb_copy_str(m->name, sizeof(m->name), method, "m");
    m->fn = fn;
    m->stream_fn = stream_fn;
    m->user = user;
    m->owns_user = owns_user;
    m->run_in_worker = run_in_worker;
    m->executor = executor;
    t = (nb_svc_task_t *)calloc(1, sizeof(*t));
    if (!t) {
        free(m);
        return NB_ERR_NOMEM;
    }
    t->node = svc->node;
    t->svc = svc;
    t->method = m;
    if (ntk_poller_async(svc->node->poller, nb_svc_method_task, t, 1) != NTK_OK) {
        free(t);
        free(m);
        return NB_ERR_IO;
    }
    return NB_OK;
}

int nb_service_add_method(nb_service_t *svc, const char *method, handler_fn fn, void *user)
{
    return nb_service_method_register(svc, method, fn, NULL, user, 0, 0, NULL);
}

int nb_service_add_method_ex(nb_service_t *svc,
                          const char *method,
                          handler_fn fn,
                          void *user,
                          const nb_service_method_options_t *options)
{
    int run_in_worker;
    const nb_service_executor_t *executor;
    int rc = nb_service_parse_exec(options, 1, &run_in_worker, &executor);
    if (rc != NB_OK) {
        return rc;
    }
    return nb_service_method_register(svc, method, fn, NULL, user, 0, run_in_worker, executor);
}

int nb_service_add_method_worker(nb_service_t *svc, const char *method, handler_fn fn, void *user)
{
    return nb_service_method_register(svc, method, fn, NULL, user, 0, 1, NULL);
}

static int nb_service_add_sync_ex(nb_service_t *svc,
                               const char *method,
                               sync_handler_fn fn,
                               void *user,
                               const nb_service_method_options_t *options)
{
    nb_sync_bridge_t *bridge;
    int run_in_worker;
    const nb_service_executor_t *executor;
    int rc;
    if (!svc || !method || !method[0] || !fn) {
        return NB_ERR_INVALID;
    }
    rc = nb_service_parse_exec(options, 1, &run_in_worker, &executor);
    if (rc != NB_OK) {
        return rc;
    }
    bridge = (nb_sync_bridge_t *)calloc(1, sizeof(*bridge));
    if (!bridge) {
        return NB_ERR_NOMEM;
    }
    bridge->fn = fn;
    bridge->user = user;
    rc = nb_service_method_register(
        svc, method, nb_sync_method_bridge, NULL, bridge, 1, run_in_worker, executor);
    if (rc != NB_OK) {
        free(bridge);
    }
    return rc;
}

int nb_service_add_sync_method(nb_service_t *svc, const char *method, sync_handler_fn fn, void *user)
{
    return nb_service_add_sync_ex(svc, method, fn, user, NULL);
}

int nb_service_add_sync_method_ex(nb_service_t *svc,
                               const char *method,
                               sync_handler_fn fn,
                               void *user,
                               const nb_service_method_options_t *options)
{
    return nb_service_add_sync_ex(svc, method, fn, user, options);
}

int nb_service_add_sync_method_worker(nb_service_t *svc,
                                   const char *method,
                                   sync_handler_fn fn,
                                   void *user)
{
    nb_service_method_options_t options = NB_SERVICE_METHOD_OPTIONS_INIT;
    options.exec = NB_SERVICE_EXEC_WORKER;
    return nb_service_add_sync_ex(svc, method, fn, user, &options);
}

int nb_service_add_stream_method(nb_service_t *svc, const char *method, chunk_handler_fn fn, void *user)
{
    return nb_service_method_register(svc, method, NULL, fn, user, 0, 0, NULL);
}

int nb_service_add_stream_method_ex(nb_service_t *svc,
                                 const char *method,
                                 chunk_handler_fn fn,
                                 void *user,
                                 const nb_service_method_options_t *options)
{
    int run_in_worker;
    const nb_service_executor_t *executor;
    int rc = nb_service_parse_exec(options, 1, &run_in_worker, &executor);
    if (rc != NB_OK) {
        return rc;
    }
    return nb_service_method_register(svc, method, NULL, fn, user, 0, run_in_worker, executor);
}
