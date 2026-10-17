/* Stub：node 之上、传输无关的调用原语（请求/事件 + 关联 + 重传）。 */
#include "node_internal.h"
#include "proxy_internal.h"

#include "envelope.h"
#include "mem.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

typedef struct zri_stub_task {
    zrpc_stub_t *stub;
    uint64_t id;
    uint64_t timeout_ms;
    zrpc_response_fn cb;
    zrpc_response_chunk_fn chunk_cb;
    zrpc_stream_done_fn done_cb;
    int streaming;
    int event;
    void *user;
    char service[ZRPC_SERVICE_NAME_MAX];
    char method[ZRPC_METHOD_NAME_MAX];
    uint8_t *data;
    size_t len;
    uint8_t encoding;
} zri_stub_task_t;

typedef struct zri_stub_resend_task {
    zrpc_node_t *node;
    uint64_t id;
} zri_stub_resend_task_t;

static void stub_task_free(zri_stub_task_t *task);

static uint8_t stub_msg_kind(zrpc_stub_kind_t kind) {
    return kind == ZRPC_STUB_EVENT ? ZRPC_KIND_EVENT : ZRPC_KIND_REQUEST;
}

/* 默认解析：静态路由表 + 服务发现。 */
static int stub_default_send(zrpc_stub_t *stub, const char *service, const char *method,
                             const zrpc_payload_t *payload, zrpc_stub_kind_t kind,
                             uint64_t request_id, zrpc_stub_target_t *target, void *user_data) {
    zrpc_node_t *node = stub->node;
    (void)user_data;
    if (zri_node_resolve(node, service, 0, target) != ZRPC_OK) {
        return ZRPC_ERR_NOTFOUND;
    }
    return zri_node_send_call(node, target, service, method, payload, stub_msg_kind(kind),
                              request_id);
}

static uint64_t stub_resend_task_fn(void *user) {
    zri_stub_resend_task_t *task = (zri_stub_resend_task_t *)user;
    zri_pending_t *pending = zri_pending_find(task->node, task->id);
    if (pending && pending->has_req) {
        zri_pending_retx(task->node, pending);
    }
    free(task);
    return 0;
}

/* 在 poller 线程内完成一次请求的发送。 */
static void stub_call_now(zrpc_stub_t *stub, uint64_t id, const char *service, const char *method,
                          const uint8_t *data, size_t len, uint8_t encoding, zrpc_response_fn cb,
                          zrpc_response_chunk_fn chunk_cb, zrpc_stream_done_fn done_cb, int streaming,
                          void *user, uint64_t timeout_ms) {
    zrpc_node_t *node = stub->node;
    zri_pending_t *p;
    zrpc_payload_t pl;
    zrpc_stub_target_t target;
    int rc;

    p = zri_pending_add(node, id, streaming ? NULL : cb, user, timeout_ms);
    if (!p) {
        if (streaming) {
            if (done_cb) {
                done_cb(ZRPC_ERR_NOMEM, user);
            }
        } else if (cb) {
            cb(ZRPC_ERR_NOMEM, NULL, user);
        }
        return;
    }
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
    if (len > 0) {
        p->data = (uint8_t *)zri_mem_alloc(len);
        if (p->data) {
            memcpy(p->data, data, len);
        } else {
            p->has_req = 0;
            p->len = 0;
        }
    }
    p->retries = 0;
    p->bp_retries = 0;
    p->next_retx_ms = zri_now_ms() + ZRPC_RTO_INIT_MS;

    pl.data = data;
    pl.len = len;
    pl.encoding = encoding;
    memset(&target, 0, sizeof(target));
    rc = stub->send(stub, service, method, &pl, ZRPC_STUB_REQUEST, id, &target, stub->user_data);
    /* 目标在解析成功时即已回填，先落进 pending 供重传使用。 */
    zri_copy_str(p->ip, sizeof(p->ip), target.ip, "0.0.0.0");
    p->port = target.port;
    p->transport = target.transport;
    zri_copy_str(p->scheme, sizeof(p->scheme), target.scheme, "udp");
    if (rc == ZRPC_OK) {
        node->metrics.requests_sent++;
        node->metrics.bytes_sent += len;
        return;
    }
    if (rc == ZRPC_ERR_AGAIN && p->bp_retries++ < 1000) {
        zri_stub_resend_task_t *retry = (zri_stub_resend_task_t *)calloc(1, sizeof(*retry));
        if (retry) {
            retry->node = node;
            retry->id = id;
            if (ztk_poller_do_delay(node->poller, 1, stub_resend_task_fn, retry) != NULL) {
                return;
            }
            free(retry);
        }
        rc = ZRPC_ERR_NOMEM;
    }
    zri_pending_invoke(p, rc, NULL);
}

static void stub_event_now(zrpc_stub_t *stub, uint64_t id, const char *service, const char *method,
                           const uint8_t *data, size_t len, uint8_t encoding) {
    zrpc_payload_t pl;
    zrpc_stub_target_t target;
    pl.data = data;
    pl.len = len;
    pl.encoding = encoding;
    memset(&target, 0, sizeof(target));
    (void)stub->send(stub, service, method, &pl, ZRPC_STUB_EVENT, id, &target, stub->user_data);
}

static void stub_call_task_fn(void *user) {
    zri_stub_task_t *t = (zri_stub_task_t *)user;
    if (t->event) {
        stub_event_now(t->stub, t->id, t->service, t->method, t->data, t->len, t->encoding);
    } else {
        stub_call_now(t->stub, t->id, t->service, t->method, t->data, t->len, t->encoding, t->cb,
                      t->chunk_cb, t->done_cb, t->streaming, t->user, t->timeout_ms);
    }
    stub_task_free(t);
}

static uint64_t stub_next_id(zrpc_node_t *node) {
    uint64_t id;
    ztk_mutex_lock(node->id_lock);
    id = (uint64_t)(uint32_t)(node->next_request_id++);
    ztk_mutex_unlock(node->id_lock);
    return id ? id : 1;
}

static void stub_task_free(zri_stub_task_t *task) {
    if (!task) {
        return;
    }
    free(task->data);
    free(task);
}

/* 创建一次调用的任务快照（service/method/payload 全部深拷贝）。 */
static zri_stub_task_t *stub_task_create(zrpc_stub_t *stub, uint64_t id, const char *service,
                                          const char *method, const uint8_t *data, size_t len,
                                          uint8_t encoding) {
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
        task->data = (uint8_t *)malloc(len);
        if (!task->data) {
            free(task);
            return NULL;
        }
        memcpy(task->data, data, len);
    }
    return task;
}

/* 跨线程投递；失败时释放任务并返回错误码。 */
static int stub_task_submit(zrpc_stub_t *stub, zri_stub_task_t *task) {
    if (ztk_poller_async(stub->node->poller, stub_call_task_fn, task, 1) != ZTK_OK) {
        stub_task_free(task);
        return ZRPC_ERR_IO;
    }
    return ZRPC_OK;
}

/* 校验并归一化 payload（长度上限、空指针、默认 encoding）。 */
static int stub_payload_prepare(const zrpc_stub_t *stub, const zrpc_payload_t *payload,
                                const uint8_t **data, size_t *len, uint8_t *encoding) {
    if (!stub || !stub->node || !data || !len || !encoding) {
        return ZRPC_ERR_INVALID;
    }
    *data = payload ? (const uint8_t *)payload->data : NULL;
    *len = payload ? payload->len : 0;
    *encoding = payload ? payload->encoding : 0;
    if (*len > stub->node->max_msg_bytes || (*len > 0 && !*data)) {
        return ZRPC_ERR_INVALID;
    }
    if (*encoding == 0) {
        *encoding = stub->payload_encoding;
    }
    return ZRPC_OK;
}

int zrpc_stub_create(zrpc_node_t *node, const zrpc_stub_options_t *options, zrpc_stub_t **out) {
    zrpc_stub_t *stub;
    if (!node || !out) {
        return ZRPC_ERR_INVALID;
    }
    *out = NULL;
    stub = (zrpc_stub_t *)calloc(1, sizeof(*stub));
    if (!stub) {
        return ZRPC_ERR_NOMEM;
    }
    stub->node = node;
    stub->payload_encoding = options ? options->payload_encoding : 0;
    stub->send = (options && options->send) ? options->send : stub_default_send;
    stub->user_data = options ? options->user_data : NULL;
    *out = stub;
    return ZRPC_OK;
}

void zrpc_stub_destroy(zrpc_stub_t *stub) {
    free(stub);
}

zrpc_node_t *zrpc_stub_node(const zrpc_stub_t *stub) {
    return stub ? stub->node : NULL;
}

int zrpc_stub_call(zrpc_stub_t *stub, const char *service, const char *method,
                   const zrpc_payload_t *payload, uint64_t *out_request_id) {
    zri_stub_task_t *t;
    uint64_t id;
    const uint8_t *data;
    size_t len;
    uint8_t encoding;
    int rc;

    if (!stub || !stub->node || !service || !method) {
        return ZRPC_ERR_INVALID;
    }
    rc = stub_payload_prepare(stub, payload, &data, &len, &encoding);
    if (rc != ZRPC_OK) {
        return rc;
    }
    id = stub_next_id(stub->node);
    if (ztk_poller_is_current_thread(stub->node->poller)) {
        stub_event_now(stub, id, service, method, data, len, encoding);
        if (out_request_id) {
            *out_request_id = id;
        }
        return ZRPC_OK;
    }
    t = stub_task_create(stub, id, service, method, data, len, encoding);
    if (!t) {
        return ZRPC_ERR_NOMEM;
    }
    t->event = 1;
    rc = stub_task_submit(stub, t);
    if (rc != ZRPC_OK) {
        return rc;
    }
    if (out_request_id) {
        *out_request_id = id;
    }
    return ZRPC_OK;
}

int zri_stub_call_ex(zrpc_stub_t *stub, const char *service, const char *method,
                      const zrpc_payload_t *payload, zrpc_response_fn cb,
                      zrpc_response_chunk_fn chunk_cb, zrpc_stream_done_fn done_cb, int streaming,
                      void *user, uint64_t timeout_ms, uint64_t *out_request_id) {
    zri_stub_task_t *t;
    uint64_t id;
    const uint8_t *data;
    size_t len;
    uint8_t encoding;
    int rc;

    if (!stub || !stub->node || !service || !method) {
        return ZRPC_ERR_INVALID;
    }
    if (!streaming && !cb) {
        return ZRPC_ERR_INVALID;
    }
    if (streaming && (!chunk_cb || !done_cb)) {
        return ZRPC_ERR_INVALID;
    }
    rc = stub_payload_prepare(stub, payload, &data, &len, &encoding);
    if (rc != ZRPC_OK) {
        return rc;
    }
    id = stub_next_id(stub->node);

    if (ztk_poller_is_current_thread(stub->node->poller)) {
        stub_call_now(stub, id, service, method, data, len, encoding, cb, chunk_cb, done_cb,
                      streaming, user, timeout_ms);
        if (out_request_id) {
            *out_request_id = id;
        }
        return ZRPC_OK;
    }

    t = stub_task_create(stub, id, service, method, data, len, encoding);
    if (!t) {
        return ZRPC_ERR_NOMEM;
    }
    t->timeout_ms = timeout_ms;
    t->cb = cb;
    t->chunk_cb = chunk_cb;
    t->done_cb = done_cb;
    t->streaming = streaming;
    t->user = user;
    rc = stub_task_submit(stub, t);
    if (rc != ZRPC_OK) {
        return rc;
    }
    if (out_request_id) {
        *out_request_id = id;
    }
    return ZRPC_OK;
}

int zrpc_stub_call_async(zrpc_stub_t *stub, const char *service, const char *method,
                         const zrpc_payload_t *payload, zrpc_response_fn cb, void *user,
                         uint64_t timeout_ms, uint64_t *out_request_id) {
    return zri_stub_call_ex(stub, service, method, payload, cb, NULL, NULL, 0, user, timeout_ms,
                             out_request_id);
}
