/* 服务端：方法注册与请求分发（回复见 reply.c）。 */
#include "node_internal.h"
#include "service_internal.h"

#include "envelope.h"
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

static void sync_method_bridge(call_t *call, const request_t *req, void *user) {
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

static void svc_link_task(void *user) {
    zri_svc_task_t *t = (zri_svc_task_t *)user;
    t->svc->next = t->node->services;
    t->node->services = t->svc;
    if (t->node->registry_backend && t->node->registry_backend->ops->announce) {
        t->node->registry_backend->ops->announce(t->node, 0);
    }
    free(t);
}

static void svc_method_task(void *user) {
    zri_svc_task_t *t = (zri_svc_task_t *)user;
    t->method->next = t->svc->methods;
    t->svc->methods = t->method;
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
    char service[SERVICE_NAME_MAX];
    char method[METHOD_NAME_MAX];
    uint8_t *payload;
    uint8_t inline_payload[HANDLER_INLINE_PAYLOAD]; /* 小请求免一次 malloc */
    size_t payload_len;
    uint8_t encoding;
    uint64_t deadline_ms; /* 请求 deadline（绝对单调 ms，0 = 无） */
    int external; /* 1 = 自定义 executor 任务 */
} zri_handler_task_t;

static void handler_inflight_release(node_t *node);
static void handler_external_release(node_t *node);

static void handler_task_release(zri_handler_task_t *t) {
    if (t->external) {
        handler_inflight_release(t->node);
        handler_external_release(t->node);
    } else {
        handler_inflight_release(t->node);
    }
}

static void handler_task_destroy(zri_handler_task_t *t) {
    if (!t) {
        return;
    }
    if (t->payload && t->payload != t->inline_payload) {
        free(t->payload);
    }
    if (t->node && t->node->handler_task_pool.lock) {
        zri_obj_pool_free(&t->node->handler_task_pool, t);
    } else {
        free(t);
    }
}

/* worker 路径的 call：优先取节点复用池，抢不到锁/池空则回退 malloc。 */
call_t *zri_worker_call_alloc(node_t *node) {
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

void zri_worker_call_free(node_t *node, call_t *call) {
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

static void handler_task_run(void *user) {
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
    rpc_log_debug("worker request %s.%s id=%llu len=%zu", t->service, t->method,
                  (unsigned long long)t->msg_id, t->payload_len);

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
static int handler_inflight_acquire(node_t *node) {
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

static void handler_inflight_release(node_t *node) {
    if (node->handler_max_concurrency == 0 || !node->handler_limit_mutex) {
        return;
    }
    ntk_mutex_lock(node->handler_limit_mutex);
    if (node->handler_inflight > 0) {
        node->handler_inflight--;
    }
    ntk_mutex_unlock(node->handler_limit_mutex);
}

static void handler_external_acquire(node_t *node) {
    if (!node->handler_limit_mutex) {
        return;
    }
    ntk_mutex_lock(node->handler_limit_mutex);
    node->external_inflight++;
    ntk_mutex_unlock(node->handler_limit_mutex);
}

static void handler_external_release(node_t *node) {
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

void zri_node_cancel_record(node_t *node, uint32_t request_id, const char *peer_ip,
                            uint16_t peer_port) {
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
                node->cancel_count--;
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
        node->cancel_count++;
    }
    ntk_mutex_unlock(node->handler_limit_mutex);
}

int zri_node_is_canceled(const node_t *node, uint32_t request_id, const char *peer_ip,
                         uint16_t peer_port) {
    int found = 0;
    int i;
    uint64_t now;
    if (!node || !node->handler_limit_mutex || node->cancel_count == 0) {
        return 0;
    }
    now = zri_now_ms();
    ntk_mutex_lock(node->handler_limit_mutex);
    for (i = 0; i < CANCEL_SLOTS; i++) {
        const zri_cancel_slot_t *s = &node->cancels[i];
        if (s->used && s->expires_ms > now && s->request_id == request_id &&
            s->peer_port == peer_port &&
            strcmp(s->peer_ip, peer_ip ? peer_ip : "") == 0) {
            found = 1;
            break;
        }
    }
    ntk_mutex_unlock(node->handler_limit_mutex);
    return found;
}

void zri_node_expire_cancels(node_t *node, uint64_t now_ms) {
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
                node->cancel_count--;
            }
        }
    }
    ntk_mutex_unlock(node->handler_limit_mutex);
}

/* 由客户端 timeout_ms 换算成绝对 deadline（接收时刻起算）。 */
static uint64_t deadline_from_timeout(uint32_t timeout_ms) {
    return timeout_ms ? zri_now_ms() + timeout_ms : 0;
}

/* 把已解析好的请求深拷贝成可跨线程执行的任务。 */
static int handler_task_create(node_t *node, zri_transport_t *transport,
                               const zri_message_t *msg, const char *service,
                               const char *method, handler_fn fn, void *user,
                               int external, zri_handler_task_t **out) {
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
    t->external = external;
    zri_copy_str(t->service, sizeof(t->service), service, "");
    zri_copy_str(t->method, sizeof(t->method), method, "");
    t->encoding = msg->payload.encoding;
    t->deadline_ms = deadline_from_timeout(msg->timeout_ms);
    t->payload_len = payload_len;
    if (payload_len > 0) {
        t->payload = payload_len <= sizeof(t->inline_payload)
                         ? t->inline_payload
                         : (uint8_t *)malloc(payload_len);
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
static int service_dispatch_worker(node_t *node, zri_transport_t *transport,
                                   const zri_message_t *msg, const char *service,
                                   const char *method, handler_fn fn, void *user) {
    zri_handler_task_t *t = NULL;
    int rc;

    rc = handler_inflight_acquire(node);
    if (rc != OK) {
        return rc;
    }
    rc = handler_task_create(node, transport, msg, service, method, fn, user, 0, &t);
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
static int service_dispatch_external(node_t *node, zri_transport_t *transport,
                                     const zri_message_t *msg, const char *service,
                                     const char *method, handler_fn fn, void *user,
                                     const service_executor_t *executor) {
    zri_handler_task_t *t = NULL;
    int rc;

    /* 自定义 executor 与内置 worker 共用同一未完成上限，避免执行器侧无界排队。 */
    rc = handler_inflight_acquire(node);
    if (rc != OK) {
        return rc;
    }
    rc = handler_task_create(node, transport, msg, service, method, fn, user, 1, &t);
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

void zri_service_handle_request(node_t *node, zri_transport_t *transport,
                                const zri_message_t *msg) {
    char tmp[MAX_ROUTE];
    char *dot;
    service_t *svc;
    zri_method_t *m;
    handler_fn fn = NULL;
    void *user = NULL;

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
    for (svc = node->services; svc; svc = svc->next) {
        if (strcmp(svc->name, tmp) != 0) {
            continue;
        }
        for (m = svc->methods; m; m = m->next) {
            if (strcmp(m->name, dot + 1) == 0 && m->fn) {
                fn = m->fn;
                user = m->user;
                break;
            }
        }
        break;
    }
    if (!fn) {
        zri_service_send_status(transport, msg, ERR_NOTFOUND);
        return;
    }
    /*
     * WORKER 方法走节点线程池。池未配置时退化为内联并告警；入池失败时直接
     * 回错误状态，避免慢 handler 在池异常时阻塞 poller。
     */
    if (m->executor) {
        int rc = service_dispatch_external(node, transport, msg, tmp, dot + 1, fn, user,
                                           m->executor);
        if (rc == OK) {
            return;
        }
        rpc_log_error("executor dispatch failed: service=%s method=%s rc=%d", tmp, dot + 1,
                      rc);
        zri_service_send_status(transport, msg, rc);
        return;
    }
    if (m->run_in_worker) {
        if (node->handler_pool) {
            int rc = service_dispatch_worker(node, transport, msg, tmp, dot + 1, fn, user);
            if (rc == OK) {
                return;
            }
            if (rc == ERR_AGAIN) {
                rpc_log_warn("worker overloaded: service=%s method=%s", tmp, dot + 1);
            } else {
                rpc_log_error("worker dispatch failed: service=%s method=%s rc=%d",
                              tmp, dot + 1, rc);
            }
            zri_service_send_status(transport, msg, rc);
            return;
        }
        if (!node->worker_fallback_warned) {
            node->worker_fallback_warned = 1;
            rpc_log_warn("handler_threads=0: worker method %s.%s runs inline on poller",
                         tmp, dot + 1);
        }
    }
    rpc_log_debug("request %s.%s id=%llu len=%zu peer=%s:%u", tmp, dot + 1,
                  (unsigned long long)msg->request_id, msg->payload.len,
                  msg->peer_ip ? msg->peer_ip : "?", (unsigned)msg->peer_port);
    {
        call_t *call = (call_t *)zri_mem_alloc(sizeof(*call));
        request_t req;
        if (!call) {
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
        fn(call, &req, user);
        if (call->one_way) {
            zri_mem_free(call);
        }
    }
}

/*
 * 客户端取消：按 request_id + 对端登记。worker 任务启动前会查表跳过；
 * 已运行的 handler 可通过 call_canceled() 观测（无法抢占）。
 */
void zri_service_handle_cancel(node_t *node, zri_transport_t *transport,
                               const zri_message_t *msg) {
    (void)transport;
    if (!node || !msg || msg->kind != KIND_CANCEL) {
        return;
    }
    zri_node_cancel_record(node, msg->request_id, msg->peer_ip, msg->peer_port);
}

uint64_t call_deadline_ms(const call_t *call) {
    return call ? call->deadline_ms : 0;
}

int call_canceled(const call_t *call) {
    if (!call) {
        return 0;
    }
    return zri_node_is_canceled(call->node, call->msg_id, call->peer_ip, call->peer_port);
}

static zri_recv_stream_t *find_recv_stream(node_t *node, const zri_message_t *msg) {
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

static zri_recv_stream_t *alloc_recv_stream(node_t *node, zri_transport_t *transport,
                                             const zri_message_t *msg) {
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

void zri_service_handle_stream_chunk(node_t *node, zri_transport_t *transport,
                                     const zri_message_t *msg) {
    char tmp[MAX_ROUTE];
    char *dot;
    service_t *svc;
    zri_method_t *method = NULL;
    zri_recv_stream_t *state;
    request_t request;
    chunk_t chunk;
    const stream_meta_t *stream_meta;

    if (!node || !msg || !msg->has_stream) {
        return;
    }
    stream_meta = &msg->stream;
    state = find_recv_stream(node, msg);

    if (stream_meta->flags & STREAM_CANCEL) {
        if (state) {
            zri_mem_free(state->call);
            memset(state, 0, sizeof(*state));
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
        for (svc = node->services; svc; svc = svc->next) {
            if (strcmp(svc->name, tmp) != 0) {
                continue;
            }
            for (method = svc->methods; method; method = method->next) {
                if (strcmp(method->name, dot + 1) == 0 && method->stream_fn) {
                    break;
                }
            }
            break;
        }
        if (!method) {
            return;
        }
        state = alloc_recv_stream(node, transport, msg);
        if (state) {
            state->method = method;
            zri_copy_str(state->service, sizeof(state->service), tmp, "");
            zri_copy_str(state->method_name, sizeof(state->method_name), dot + 1, "");
        }
    }
    if (!state || !state->call || !method) {
        return;
    }
    state->last_ms = zri_now_ms();
    state->bytes += msg->payload.len;
    request.service = state->service;
    request.method = state->method_name;
    request.payload = msg->payload;
    request.peer_ip = state->call->peer_ip;
    request.peer_port = msg->peer_port;
    request.deadline_ms = state->call->deadline_ms;
    chunk.data = msg->payload.data;
    chunk.len = msg->payload.len;
    chunk.encoding = msg->payload.encoding;
    chunk.offset = stream_meta->offset;
    chunk.flags = stream_meta->flags;
    method->stream_fn(state->call, &request, &chunk, method->user);

    if (stream_meta->flags & STREAM_LAST) {
        zri_mem_free(state->call);
        memset(state, 0, sizeof(*state));
    }
}

void zri_service_expire_streams(node_t *node, uint64_t now_ms) {
    int i;
    if (!node) {
        return;
    }
    for (i = 0; i < MAX_RECV_STREAMS; i++) {
        zri_recv_stream_t *stream = &node->recv_streams[i];
        if (stream->used && now_ms - stream->last_ms > STREAM_IDLE_TIMEOUT_MS) {
            zri_mem_free(stream->call);
            memset(stream, 0, sizeof(*stream));
        }
    }
}

/* ---- 服务生命周期 ---- */

int service_create(node_t *node, const char *service_name, service_t **out) {
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

void service_destroy(service_t *svc) {
    /* v1：不在运行期注销；节点销毁时统一释放。 */
    (void)svc;
}

/* ---- 方法注册 ---- */

/*
 * 解析调用者选择的执行模式。allow_worker 为 0 时只允许 INLINE（流式方法）。
 * 输出 run_in_worker / executor 二选一。
 */
static int service_parse_exec(const service_method_options_t *options, int allow_worker,
                              int *out_worker, const service_executor_t **out_executor) {
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

static int service_method_register(service_t *svc, const char *method, handler_fn fn,
                                   chunk_handler_fn stream_fn, void *user, int owns_user,
                                   int run_in_worker, const service_executor_t *executor) {
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

int service_add_method(service_t *svc, const char *method, handler_fn fn, void *user) {
    return service_method_register(svc, method, fn, NULL, user, 0, 0, NULL);
}

int service_add_method_ex(service_t *svc, const char *method, handler_fn fn, void *user,
                          const service_method_options_t *options) {
    int run_in_worker;
    const service_executor_t *executor;
    int rc = service_parse_exec(options, 1, &run_in_worker, &executor);
    if (rc != OK) {
        return rc;
    }
    return service_method_register(svc, method, fn, NULL, user, 0, run_in_worker, executor);
}

int service_add_method_worker(service_t *svc, const char *method, handler_fn fn, void *user) {
    return service_method_register(svc, method, fn, NULL, user, 0, 1, NULL);
}

static int service_add_sync_ex(service_t *svc, const char *method, sync_handler_fn fn,
                               void *user, const service_method_options_t *options) {
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
    rc = service_method_register(svc, method, sync_method_bridge, NULL, bridge, 1,
                                 run_in_worker, executor);
    if (rc != OK) {
        free(bridge);
    }
    return rc;
}

int service_add_sync_method(service_t *svc, const char *method, sync_handler_fn fn,
                            void *user) {
    return service_add_sync_ex(svc, method, fn, user, NULL);
}

int service_add_sync_method_ex(service_t *svc, const char *method, sync_handler_fn fn,
                               void *user, const service_method_options_t *options) {
    return service_add_sync_ex(svc, method, fn, user, options);
}

int service_add_sync_method_worker(service_t *svc, const char *method, sync_handler_fn fn,
                                   void *user) {
    service_method_options_t options = SERVICE_METHOD_OPTIONS_INIT;
    options.exec = SERVICE_EXEC_WORKER;
    return service_add_sync_ex(svc, method, fn, user, &options);
}

int service_add_stream_method(service_t *svc, const char *method,
                              chunk_handler_fn fn, void *user) {
    return service_method_register(svc, method, NULL, fn, user, 0, 0, NULL);
}

int service_add_stream_method_ex(service_t *svc, const char *method, chunk_handler_fn fn,
                                 void *user, const service_method_options_t *options) {
    int run_in_worker;
    const service_executor_t *executor;
    int rc = service_parse_exec(options, 0, &run_in_worker, &executor);
    if (rc != OK) {
        return rc;
    }
    return service_method_register(svc, method, NULL, fn, user, 0, run_in_worker, executor);
}
