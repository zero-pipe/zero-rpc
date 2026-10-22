/* Stub：node 之上、传输无关的调用原语（请求/事件 + 关联 + 重传）。 */
#include "node_internal.h"
#include "proxy_internal.h"

#include "envelope.h"
#include "atomic.h"
#include "mem.h"
#include "util.h"

#include <log.h>

#include <stdlib.h>
#include <string.h>

typedef struct zri_stub_task {
    stub_t *stub;
    uint64_t id;
    uint64_t timeout_ms;
    response_fn cb;
    response_chunk_fn chunk_cb;
    stream_done_fn done_cb;
    int streaming;
    int event;
    void *user;
    char service[SERVICE_NAME_MAX];
    char method[METHOD_NAME_MAX];
    uint8_t *data;
    uint8_t *metadata;
    uint16_t metadata_len;
    size_t len;
    uint8_t encoding;
} zri_stub_task_t;

typedef struct zri_stub_resend_task {
    node_t *node;
    uint64_t id;
} zri_stub_resend_task_t;

static void stub_task_free(zri_stub_task_t *task);

static uint8_t stub_msg_kind(stub_kind_t kind)
{
    return kind == STUB_EVENT ? KIND_EVENT : KIND_REQUEST;
}

/* 默认解析：静态路由表 + 服务发现。 */
static int stub_default_send(stub_t *stub,
                             const char *service,
                             const char *method,
                             const payload_t *payload,
                             stub_kind_t kind,
                             uint64_t request_id,
                             uint64_t timeout_ms,
                             const void *metadata,
                             uint16_t metadata_len,
                             stub_target_t *target,
                             void *user_data)
{
    node_t *node = stub->node;
    (void)user_data;
    if (zri_node_resolve(node, service, 0, target) != OK) {
        return ERR_NOTFOUND;
    }
    return zri_node_send_call(node,
                              target,
                              service,
                              method,
                              payload,
                              stub_msg_kind(kind),
                              request_id,
                              timeout_ms,
                              metadata,
                              metadata_len);
}

static uint64_t stub_resend_task_fn(void *user)
{
    zri_stub_resend_task_t *task = (zri_stub_resend_task_t *)user;
    zri_pending_t *pending = zri_pending_find(task->node, task->id);
    if (pending && pending->has_req) {
        zri_pending_retx(task->node, pending);
    }
    free(task);
    return 0;
}

/* 在 poller 线程内完成一次请求的发送。 */
static void stub_call_now(stub_t *stub,
                          zri_stub_task_t *owner,
                          uint64_t id,
                          const char *service,
                          const char *method,
                          const uint8_t *data,
                          size_t len,
                          uint8_t encoding,
                          response_fn cb,
                          response_chunk_fn chunk_cb,
                          stream_done_fn done_cb,
                          int streaming,
                          void *user,
                          uint64_t timeout_ms,
                          const uint8_t *metadata,
                          uint16_t metadata_len)
{
    node_t *node = stub->node;
    zri_pending_t *p;
    payload_t pl;
    stub_target_t target;
    int rc;
    int prc = OK;

    p = zri_pending_add(
        node, id, streaming ? NULL : cb, user, timeout_ms, ntk_poller_current(), &prc);
    if (!p) {
        int err = prc == OK ? ERR_NOMEM : prc;
        if (streaming) {
            if (done_cb) {
                done_cb(err, user);
            }
        } else if (cb) {
            cb(err, NULL, user);
        }
        return;
    }
    p->owner = stub;
    p->streaming = streaming;
    p->chunk_cb = chunk_cb;
    p->done_cb = done_cb;
    p->stream_user = user;
    p->has_req = 1;
    zri_copy_str(p->service, sizeof(p->service), service, "");
    zri_copy_str(p->method, sizeof(p->method), method, "");
    p->encoding = encoding;
    p->len = len;
    p->data = NULL;
    p->metadata = NULL;
    p->metadata_len = 0;
    if (len > 0) {
        if (owner) {
            /* 异步任务已持有一份拷贝：直接转移所有权，避免第二次整包拷贝。 */
            p->data = owner->data;
            owner->data = NULL;
        } else {
            p->data = (uint8_t *)zri_mem_alloc(len);
            if (p->data) {
                memcpy(p->data, data, len);
            }
        }
        if (!p->data) {
            p->has_req = 0;
            p->len = 0;
        }
    }
    if (metadata_len > 0 && metadata) {
        if (owner) {
            p->metadata = owner->metadata;
            owner->metadata = NULL;
        } else {
            p->metadata = (uint8_t *)malloc(metadata_len);
            if (p->metadata) {
                memcpy(p->metadata, metadata, metadata_len);
            }
        }
        if (p->metadata) {
            p->metadata_len = metadata_len;
        }
    }
    p->retries = 0;
    p->bp_retries = 0;
    p->next_retx_ms = zri_now_ms() + RTO_INIT_MS;
    rpc_log_debug("call %s.%s id=%llu len=%zu timeout=%llu",
                  service,
                  method,
                  (unsigned long long)id,
                  len,
                  (unsigned long long)timeout_ms);

    pl.data = data;
    pl.len = len;
    pl.encoding = encoding;
    memset(&target, 0, sizeof(target));
    rc = stub->send(stub,
                    service,
                    method,
                    &pl,
                    STUB_REQUEST,
                    id,
                    timeout_ms,
                    metadata,
                    metadata_len,
                    &target,
                    stub->user_data);
    /* 目标在解析成功时即已回填，先落进 pending 供重传使用。 */
    zri_copy_str(p->ip, sizeof(p->ip), target.ip, "0.0.0.0");
    p->port = target.port;
    p->transport = target.transport;
    zri_copy_str(p->scheme, sizeof(p->scheme), target.scheme, "udp");
    if (rc == OK) {
        ZRI_METRIC_INC(node->metrics.requests_sent);
        ZRI_METRIC_ADD(node->metrics.bytes_sent, len);
        return;
    }
    if (rc == ERR_AGAIN && p->bp_retries++ < 1000) {
        zri_stub_resend_task_t *retry = (zri_stub_resend_task_t *)calloc(1, sizeof(*retry));
        if (retry) {
            ntk_poller *poller = ntk_poller_current();
            retry->node = node;
            retry->id = id;
            if (ntk_poller_do_delay(poller ? poller : node->poller, 1, stub_resend_task_fn, retry) !=
                NULL) {
                return;
            }
            free(retry);
        }
        rc = ERR_NOMEM;
    }
    zri_pending_invoke(p, rc, NULL);
}

static void stub_event_now(stub_t *stub,
                           uint64_t id,
                           const char *service,
                           const char *method,
                           const uint8_t *data,
                           size_t len,
                           uint8_t encoding,
                           const uint8_t *metadata,
                           uint16_t metadata_len)
{
    payload_t pl;
    stub_target_t target;
    pl.data = data;
    pl.len = len;
    pl.encoding = encoding;
    memset(&target, 0, sizeof(target));
    (void)stub->send(stub,
                     service,
                     method,
                     &pl,
                     STUB_EVENT,
                     id,
                     0,
                     metadata,
                     metadata_len,
                     &target,
                     stub->user_data);
}

static void stub_call_task_fn(void *user)
{
    zri_stub_task_t *t = (zri_stub_task_t *)user;
    if (t->event) {
        stub_event_now(t->stub,
                       t->id,
                       t->service,
                       t->method,
                       t->data,
                       t->len,
                       t->encoding,
                       t->metadata,
                       t->metadata_len);
    } else {
        stub_call_now(t->stub,
                      t,
                      t->id,
                      t->service,
                      t->method,
                      t->data,
                      t->len,
                      t->encoding,
                      t->cb,
                      t->chunk_cb,
                      t->done_cb,
                      t->streaming,
                      t->user,
                      t->timeout_ms,
                      t->metadata,
                      t->metadata_len);
    }
    stub_task_free(t);
}

static uint64_t stub_next_id(node_t *node)
{
    uint64_t id;
    ntk_mutex_lock(node->id_lock);
    id = (uint64_t)(uint32_t)(node->next_request_id++);
    ntk_mutex_unlock(node->id_lock);
    return id ? id : 1;
}

static void stub_task_free(zri_stub_task_t *task)
{
    if (!task) {
        return;
    }
    zri_mem_free(task->data);
    free(task->metadata);
    free(task);
}

/* 创建一次调用的任务快照（service/method/payload 全部深拷贝）。 */
static zri_stub_task_t *stub_task_create(stub_t *stub,
                                         uint64_t id,
                                         const char *service,
                                         const char *method,
                                         const uint8_t *data,
                                         size_t len,
                                         uint8_t encoding,
                                         const uint8_t *metadata,
                                         uint16_t metadata_len)
{
    zri_stub_task_t *task;

    task = (zri_stub_task_t *)calloc(1, sizeof(*task));
    if (!task) {
        return NULL;
    }
    task->stub = stub;
    task->id = id;
    zri_copy_str(task->service, sizeof(task->service), service, "");
    zri_copy_str(task->method, sizeof(task->method), method, "");
    task->encoding = encoding;
    task->len = len;
    if (len > 0) {
        /* 与 pending 的释放方式保持一致：可能被 pending 直接接管。 */
        task->data = (uint8_t *)zri_mem_alloc(len);
        if (!task->data) {
            free(task);
            return NULL;
        }
        memcpy(task->data, data, len);
    }
    if (metadata_len > 0 && metadata) {
        task->metadata = (uint8_t *)malloc(metadata_len);
        if (!task->metadata) {
            stub_task_free(task);
            return NULL;
        }
        memcpy(task->metadata, metadata, metadata_len);
        task->metadata_len = metadata_len;
    }
    return task;
}

/* 跨线程投递；失败时释放任务并返回错误码。 */
static int stub_task_submit(stub_t *stub, zri_stub_task_t *task)
{
    ntk_poller *poller = zri_node_pick_poller(stub->node, task->id);
    if (ntk_poller_async(poller, stub_call_task_fn, task, 1) != NTK_OK) {
        stub_task_free(task);
        return ERR_IO;
    }
    return OK;
}

/* 校验并归一化 payload（长度上限、空指针、默认 encoding）。 */
static int stub_payload_prepare(const stub_t *stub,
                                const payload_t *payload,
                                const uint8_t **data,
                                size_t *len,
                                uint8_t *encoding)
{
    if (!stub || !stub->node || !data || !len || !encoding) {
        return ERR_INVALID;
    }
    *data = payload ? (const uint8_t *)payload->data : NULL;
    *len = payload ? payload->len : 0;
    *encoding = payload ? payload->encoding : 0;
    if (*len > stub->node->max_msg_bytes || (*len > 0 && !*data)) {
        return ERR_INVALID;
    }
    if (*encoding == 0) {
        *encoding = stub->payload_encoding;
    }
    return OK;
}

/* 把 metadata 编码进调用方的 inline 线格式缓冲；无 metadata 时 out_len = 0。 */
static int stub_metadata_encode(const metadata_t *metadata,
                                uint8_t *buf,
                                size_t cap,
                                uint16_t *out_len)
{
    size_t len = 0;
    *out_len = 0;
    if (!metadata) {
        return OK;
    }
    if (metadata_encode(metadata, buf, cap, &len) != OK) {
        return ERR_TOOBIG;
    }
    *out_len = (uint16_t)len;
    return OK;
}

/* ---- 生命周期 ---- */

int stub_create(node_t *node, const stub_options_t *options, stub_t **out)
{
    stub_t *stub;
    if (!node || !out) {
        return ERR_INVALID;
    }
    *out = NULL;
    stub = (stub_t *)calloc(1, sizeof(*stub));
    if (!stub) {
        return ERR_NOMEM;
    }
    stub->node = node;
    stub->payload_encoding = options ? options->payload_encoding : 0;
    stub->send = (options && options->send) ? options->send : stub_default_send;
    stub->user_data = options ? options->user_data : NULL;
    *out = stub;
    return OK;
}

void stub_destroy(stub_t *stub)
{
    free(stub);
}

node_t *stub_node(const stub_t *stub)
{
    return stub ? stub->node : NULL;
}

/* ---- 调用 ---- */

int stub_call_ex(stub_t *stub,
                 const char *service,
                 const char *method,
                 const payload_t *payload,
                 const metadata_t *metadata,
                 uint64_t *out_request_id)
{
    zri_stub_task_t *t;
    uint64_t id;
    const uint8_t *data;
    size_t len;
    uint8_t encoding;
    uint8_t metadata_buf[METADATA_WIRE_MAX];
    uint16_t metadata_len = 0;
    int rc;

    if (!stub || !stub->node || !service || !method) {
        return ERR_INVALID;
    }
    rc = stub_payload_prepare(stub, payload, &data, &len, &encoding);
    if (rc != OK) {
        return rc;
    }
    if (stub_metadata_encode(metadata, metadata_buf, sizeof(metadata_buf), &metadata_len) != OK) {
        rpc_log_error("metadata encode failed (too large?): service=%s method=%s", service, method);
        return ERR_TOOBIG;
    }
    id = stub_next_id(stub->node);
    if (ntk_poller_is_current_thread(zri_node_pick_poller(stub->node, id))) {
        stub_event_now(stub, id, service, method, data, len, encoding, metadata_buf, metadata_len);
        if (out_request_id) {
            *out_request_id = id;
        }
        return OK;
    }
    t = stub_task_create(
        stub, id, service, method, data, len, encoding, metadata_buf, metadata_len);
    if (!t) {
        return ERR_NOMEM;
    }
    t->event = 1;
    rc = stub_task_submit(stub, t);
    if (rc != OK) {
        return rc;
    }
    if (out_request_id) {
        *out_request_id = id;
    }
    return OK;
}

int stub_call(stub_t *stub,
              const char *service,
              const char *method,
              const payload_t *payload,
              uint64_t *out_request_id)
{
    return stub_call_ex(stub, service, method, payload, NULL, out_request_id);
}

int zri_stub_call_ex(stub_t *stub,
                     const char *service,
                     const char *method,
                     const payload_t *payload,
                     response_fn cb,
                     response_chunk_fn chunk_cb,
                     stream_done_fn done_cb,
                     int streaming,
                     void *user,
                     uint64_t timeout_ms,
                     uint64_t *out_request_id,
                     const metadata_t *metadata)
{
    zri_stub_task_t *t;
    uint64_t id;
    const uint8_t *data;
    size_t len;
    uint8_t encoding;
    uint8_t metadata_buf[METADATA_WIRE_MAX];
    uint16_t metadata_len = 0;
    int rc;

    if (!stub || !stub->node || !service || !method) {
        return ERR_INVALID;
    }
    if (!streaming && !cb) {
        return ERR_INVALID;
    }
    if (streaming && (!chunk_cb || !done_cb)) {
        return ERR_INVALID;
    }
    rc = stub_payload_prepare(stub, payload, &data, &len, &encoding);
    if (rc != OK) {
        return rc;
    }
    if (stub_metadata_encode(metadata, metadata_buf, sizeof(metadata_buf), &metadata_len) != OK) {
        rpc_log_error("metadata encode failed (too large?): service=%s method=%s", service, method);
        return ERR_TOOBIG;
    }
    id = stub_next_id(stub->node);

    if (ntk_poller_is_current_thread(zri_node_pick_poller(stub->node, id))) {
        stub_call_now(stub,
                      NULL,
                      id,
                      service,
                      method,
                      data,
                      len,
                      encoding,
                      cb,
                      chunk_cb,
                      done_cb,
                      streaming,
                      user,
                      timeout_ms,
                      metadata_buf,
                      metadata_len);
        if (out_request_id) {
            *out_request_id = id;
        }
        return OK;
    }

    t = stub_task_create(
        stub, id, service, method, data, len, encoding, metadata_buf, metadata_len);
    if (!t) {
        return ERR_NOMEM;
    }
    t->timeout_ms = timeout_ms;
    t->cb = cb;
    t->chunk_cb = chunk_cb;
    t->done_cb = done_cb;
    t->streaming = streaming;
    t->user = user;
    rc = stub_task_submit(stub, t);
    if (rc != OK) {
        return rc;
    }
    if (out_request_id) {
        *out_request_id = id;
    }
    return OK;
}

int stub_call_async(stub_t *stub,
                    const char *service,
                    const char *method,
                    const payload_t *payload,
                    response_fn cb,
                    void *user,
                    uint64_t timeout_ms,
                    uint64_t *out_request_id)
{
    return zri_stub_call_ex(
        stub, service, method, payload, cb, NULL, NULL, 0, user, timeout_ms, out_request_id, NULL);
}

int stub_call_async_ex(stub_t *stub,
                       const char *service,
                       const char *method,
                       const payload_t *payload,
                       const metadata_t *metadata,
                       response_fn cb,
                       void *user,
                       uint64_t timeout_ms,
                       uint64_t *out_request_id)
{
    return zri_stub_call_ex(stub,
                            service,
                            method,
                            payload,
                            cb,
                            NULL,
                            NULL,
                            0,
                            user,
                            timeout_ms,
                            out_request_id,
                            metadata);
}

/* ---- 批量取消（销毁前调用，避免回调访问已释放对象） ---- */

/*
 * 批量取消：pending 分散在各 poller 的 lane 上，只允许归属 poller 取消自己的
 * lane。ctx 由各 poller 的取消任务共享（原子 remaining + sem），全部完成后由
 * 调用方回收；超时不回收，避免任务仍在引用 ctx 时 UAF。
 */
typedef struct zri_stub_cancel_ctx {
    stub_t *stub;
    node_t *node;
    int status;
    ntk_sem *sem;
    uint32_t remaining;
} zri_stub_cancel_ctx_t;

typedef struct zri_stub_cancel_task {
    zri_stub_cancel_ctx_t *ctx;
} zri_stub_cancel_task_t;

/* 在 poller 线程内取消本 lane 上属于该 stub 的 pending。 */
static void stub_cancel_lane_now(zri_stub_cancel_ctx_t *ctx, int idx)
{
    zri_pending_t *p;
    if (idx >= 0) {
        p = ctx->node->pending_lanes[idx];
        while (p) {
            zri_pending_t *next = p->lane_next;
            if (p->used && p->owner == ctx->stub) {
                zri_pending_invoke(p, ctx->status, NULL);
            }
            p = next;
        }
    }
    if (zri_atomic_sub_u32(&ctx->remaining, 1u) == 1u) {
        if (ctx->sem) {
            ntk_sem_post(ctx->sem, 1);
        }
    }
}

static void stub_cancel_lane_fn(void *user)
{
    zri_stub_cancel_task_t *t = (zri_stub_cancel_task_t *)user;
    zri_stub_cancel_ctx_t *ctx = t->ctx;
    int idx = zri_node_poller_index(ctx->node, ntk_poller_current());
    stub_cancel_lane_now(ctx, idx);
    free(t);
}

int zri_stub_cancel_all(stub_t *stub, int status)
{
    node_t *node;
    zri_stub_cancel_ctx_t *ctx;
    ntk_sem *sem;
    unsigned i;
    int cur_idx;
    if (!stub || !(node = stub->node)) {
        return ERR_INVALID;
    }
    if (node->shutting_down) {
        return OK; /* poller 已停，回调不会再运行，无需取消 */
    }
    if (node->io_count == 0) {
        return OK;
    }
    sem = ntk_sem_create(0);
    if (!sem) {
        return ERR_NOMEM;
    }
    ctx = (zri_stub_cancel_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        ntk_sem_destroy(sem);
        return ERR_NOMEM;
    }
    ctx->stub = stub;
    ctx->node = node;
    ctx->status = status;
    ctx->sem = sem;
    ctx->remaining = node->io_count;

    /* 当前线程若本身是某个 poller，则本 lane 内联处理，避免自等待死锁。 */
    cur_idx = zri_node_poller_index(node, ntk_poller_current());
    if (cur_idx >= 0) {
        stub_cancel_lane_now(ctx, cur_idx);
    }
    for (i = 0; i < node->io_count; i++) {
        zri_stub_cancel_task_t *t;
        if ((int)i == cur_idx) {
            continue;
        }
        t = (zri_stub_cancel_task_t *)calloc(1, sizeof(*t));
        if (!t) {
            zri_atomic_sub_u32(&ctx->remaining, 1u);
            continue;
        }
        t->ctx = ctx;
        if (ntk_poller_async(node->io_pollers[i], stub_cancel_lane_fn, t, 1) != NTK_OK) {
            free(t);
            zri_atomic_sub_u32(&ctx->remaining, 1u);
        }
    }
    if (zri_atomic_load_u32(&ctx->remaining) == 0) {
        ntk_sem_destroy(sem);
        free(ctx);
        return OK;
    }
    if (ntk_sem_timedwait(sem, 5000) != NTK_OK) {
        /* 超时：任务可能仍在执行，sem/ctx 交给最后完成的任务回收，避免 UAF。 */
        return ERR_TIMEOUT;
    }
    ntk_sem_destroy(sem);
    free(ctx);
    return OK;
}
