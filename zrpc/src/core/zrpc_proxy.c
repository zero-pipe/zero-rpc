/* 客户端：代理对象、路由解析、异步调用与取消。 */
#include "zrpc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct zrpc_call_task {
    zrpc_proxy_t *proxy;
    uint64_t id;
    uint64_t timeout_ms;
    zrpc_response_fn cb;
    zrpc_response_chunk_fn chunk_cb;
    zrpc_stream_done_fn done_cb;
    int streaming;
    void *user;
    char service[ZRPC_SERVICE_NAME_MAX];
    char method[ZRPC_METHOD_NAME_MAX];
    uint8_t *data;
    size_t len;
    uint8_t encoding;
} zrpc_call_task_t;

typedef struct zrpc_cancel_task {
    zrpc_proxy_t *proxy;
    uint64_t id;
} zrpc_cancel_task_t;

typedef struct zrpc_stream_open_task {
    zrpc_stream_t *stream;
} zrpc_stream_open_task_t;

typedef struct zrpc_stream_send_task {
    zrpc_stream_t *stream;
    uint8_t *data;
    size_t len;
    uint8_t encoding;
    uint64_t offset;
    unsigned flags;
    int first;
    int retries;
} zrpc_stream_send_task_t;

typedef struct zrpc_resend_task {
    zrpc_node_t *node;
    uint64_t id;
} zrpc_resend_task_t;

static uint64_t stream_send_task_fn(void *user);

static void stream_send_async_fn(void *user) {
    (void)stream_send_task_fn(user);
}

static uint64_t resend_task_fn(void *user) {
    zrpc_resend_task_t *task = (zrpc_resend_task_t *)user;
    zrpc_pending_t *pending = zrpc_pending_find(task->node, task->id);
    if (pending && pending->has_req) {
        zrpc_pending_retx(task->node, pending);
    }
    free(task);
    return 0;
}

static void stream_response(int status, const zrpc_response_t *resp, void *user) {
    zrpc_stream_t *stream = (zrpc_stream_t *)user;
    if (!stream) {
        return;
    }
    stream->closed = 1;
    if (stream->cb) {
        stream->cb(status, resp, stream->user);
    }
}

static void stream_bidi_chunk(int status, const zrpc_chunk_t *chunk, void *user) {
    zrpc_stream_t *stream = (zrpc_stream_t *)user;
    if (stream && stream->chunk_cb) {
        stream->chunk_cb(status, chunk, stream->user);
    }
}

static void stream_bidi_done(int status, void *user) {
    zrpc_stream_t *stream = (zrpc_stream_t *)user;
    if (!stream) {
        return;
    }
    stream->closed = 1;
    if (stream->done_cb) {
        stream->done_cb(status, stream->user);
    }
}

static void stream_open_task_fn(void *user) {
    zrpc_stream_open_task_t *task = (zrpc_stream_open_task_t *)user;
    zrpc_stream_t *stream = task->stream;
    zrpc_pending_t *pending;
    if (stream->closed) {
        free(task);
        return;
    }
    pending = zrpc_pending_add(stream->node, stream->request_id,
                               stream->bidi ? NULL : stream_response, stream, stream->timeout_ms);
    if (!pending) {
        stream_response(ZRPC_ERR_NOMEM, NULL, stream);
    } else {
        if (stream->bidi) {
            pending->streaming = 1;
            pending->chunk_cb = stream_bidi_chunk;
            pending->done_cb = stream_bidi_done;
            pending->stream_user = stream;
        }
        stream->opened = 1;
    }
    free(task);
}

static uint64_t stream_send_task_fn(void *user) {
    zrpc_stream_send_task_t *task = (zrpc_stream_send_task_t *)user;
    zrpc_stream_t *stream = task->stream;
    zrpc_payload_t payload;
    zrpc_stream_meta_t meta;
    char route[ZRPC_SERVICE_NAME_MAX + ZRPC_METHOD_NAME_MAX + 2];
    int rc;
    if (stream->closed) {
        free(task->data);
        free(task);
        return 0;
    }
    if (task->first) {
        snprintf(route, sizeof(route), "%s.%s", stream->service, stream->method);
    } else {
        route[0] = '\0';
    }
    payload.data = task->data;
    payload.len = task->len;
    payload.encoding = task->encoding;
    meta.flags = task->flags;
    meta.stream_id = stream->stream_id;
    meta.offset = task->offset;
    meta.total_size = 0;
    rc = zrpc_node_send(stream->node, stream->ip, stream->port, ZRPC_KIND_REQUEST,
                        (uint32_t)stream->request_id, stream->transport, stream->scheme, route,
                        &payload, &meta);
    if (rc == ZRPC_ERR_AGAIN && task->retries++ < 1000) {
        if (ztk_poller_do_delay(stream->node->poller, 1, stream_send_task_fn, task) != NULL) {
            return 0; /* 保留 task/data，稍后重试 */
        }
    }
    if (rc != ZRPC_OK) {
        zrpc_pending_t *pending = zrpc_pending_find(stream->node, stream->request_id);
        if (pending) {
            zrpc_pending_invoke(pending, rc, NULL);
        }
    }
    free(task->data);
    free(task);
    return 0;
}

static void stream_cancel_task_fn(void *user) {
    zrpc_stream_t *stream = (zrpc_stream_t *)user;
    zrpc_pending_t *pending;
    if (!stream || stream->closed) {
        return;
    }
    {
        zrpc_payload_t empty;
        zrpc_stream_meta_t meta;
        char route[ZRPC_SERVICE_NAME_MAX + ZRPC_METHOD_NAME_MAX + 2];
        memset(&empty, 0, sizeof(empty));
        memset(&meta, 0, sizeof(meta));
        meta.flags = ZRPC_STREAM_CANCEL;
        meta.stream_id = stream->stream_id;
        snprintf(route, sizeof(route), "%s.%s", stream->service, stream->method);
        (void)zrpc_node_send(stream->node, stream->ip, stream->port, ZRPC_KIND_REQUEST,
                             (uint32_t)stream->request_id, stream->transport, stream->scheme, route,
                             &empty, &meta);
    }
    pending = zrpc_pending_find(stream->node, stream->request_id);
    if (pending) {
        zrpc_pending_invoke(pending, ZRPC_ERR_CLOSED, NULL);
    } else {
        stream->closed = 1;
    }
}

int zrpc_proxy_resolve(zrpc_proxy_t *proxy, const char *service, char *ip_out, size_t ip_cap,
                       uint16_t *port_out, zrpc_transport_kind_t *transport_out,
                       char *scheme_out, size_t scheme_cap, int prefer_stream) {
    zrpc_node_t *node = proxy->node;
    zrpc_transport_kind_t kind;
    if (proxy->has_endpoint) {
        zrpc_copy_str(ip_out, ip_cap, proxy->endpoint_ip, "127.0.0.1");
        *port_out = proxy->endpoint_port;
        if (transport_out) {
            *transport_out = proxy->endpoint_transport;
        }
        if (scheme_out) {
            zrpc_copy_str(scheme_out, scheme_cap, proxy->endpoint_scheme[0] ? proxy->endpoint_scheme
                                                                            : (proxy->endpoint_transport ==
                                                                                       ZRPC_TRANSPORT_TCP
                                                                                   ? "tcp"
                                                                                   : "udp"),
                          "udp");
        }
        return ZRPC_OK;
    }
    if (zrpc_route_lookup_prefer(node, service, prefer_stream, ip_out, ip_cap, port_out, &kind,
                                 scheme_out, scheme_cap) == ZRPC_OK) {
        if (transport_out) {
            *transport_out = kind;
        }
        return ZRPC_OK;
    }
    {
        zrpc_endpoint_t endpoint;
        if (zrpc_registry_select_prefer(node, service, prefer_stream, &endpoint) != ZRPC_OK) {
            return ZRPC_ERR_NOTFOUND;
        }
        zrpc_copy_str(ip_out, ip_cap, endpoint.host, "0.0.0.0");
        *port_out = endpoint.port;
        if (transport_out) {
            *transport_out = endpoint.transport;
        }
        if (scheme_out) {
            zrpc_copy_str(scheme_out, scheme_cap, endpoint.scheme, "udp");
        }
    }
    return ZRPC_OK;
}

/* 在 poller 线程内完成一次调用的发送。 */
static void call_now(zrpc_node_t *node, zrpc_proxy_t *proxy, uint64_t id, const char *service,
                     const char *method, const uint8_t *data, size_t len, uint8_t encoding,
                     zrpc_response_fn cb, zrpc_response_chunk_fn chunk_cb,
                     zrpc_stream_done_fn done_cb, int streaming, void *user, uint64_t timeout_ms) {
    char ip[ZRPC_ENDPOINT_MAX];
    uint16_t port = 0;
    char route[ZRPC_SERVICE_NAME_MAX + ZRPC_METHOD_NAME_MAX + 2];
    zrpc_payload_t pl;
    zrpc_pending_t *p;
    zrpc_transport_kind_t transport_kind;
    char scheme[ZRPC_SCHEME_MAX];
    int rc;

    if (zrpc_proxy_resolve(proxy, service, ip, sizeof(ip), &port, &transport_kind, scheme,
                           sizeof(scheme), proxy->prefer_stream_threshold > 0 &&
                                               len > proxy->prefer_stream_threshold) != ZRPC_OK) {
        if (cb) {
            cb(ZRPC_ERR_NOTFOUND, NULL, user);
        }
        return;
    }
    p = zrpc_pending_add(node, id, streaming ? NULL : cb, user, timeout_ms);
    if (!p) {
        if (cb) {
            cb(ZRPC_ERR_NOMEM, NULL, user);
        }
        return;
    }
    p->streaming = streaming;
    p->chunk_cb = chunk_cb;
    p->done_cb = done_cb;
    p->stream_user = user;
    /* 保存请求以支持 RTO 重传 */
    p->has_req = 1;
    zrpc_copy_str(p->service, sizeof(p->service), service, "");
    zrpc_copy_str(p->method, sizeof(p->method), method, "");
    zrpc_copy_str(p->ip, sizeof(p->ip), ip, "0.0.0.0");
    p->port = port;
    p->transport = transport_kind;
    zrpc_copy_str(p->scheme, sizeof(p->scheme), scheme, "udp");
    p->encoding = encoding;
    p->len = len;
    p->data = NULL;
    if (len > 0) {
        p->data = (uint8_t *)malloc(len);
        if (p->data) {
            memcpy(p->data, data, len);
        } else {
            p->has_req = 0;
            p->len = 0;
        }
    }
    p->retries = 0;
    p->next_retx_ms = zrpc_now_ms() + ZRPC_RTO_INIT_MS;
    snprintf(route, sizeof(route), "%s.%s", service, method);
    pl.data = data;
    pl.len = len;
    pl.encoding = encoding;
    rc = zrpc_node_send(node, ip, port, ZRPC_KIND_REQUEST, (uint32_t)id, transport_kind, scheme,
                        route, &pl, NULL);
    if (rc != ZRPC_OK) {
        if (rc == ZRPC_ERR_AGAIN && p->bp_retries++ < 1000) {
            zrpc_resend_task_t *retry = (zrpc_resend_task_t *)calloc(1, sizeof(*retry));
            if (retry) {
                retry->node = node;
                retry->id = id;
                if (ztk_poller_do_delay(node->poller, 1, resend_task_fn, retry) == NULL) {
                    free(retry);
                    zrpc_pending_invoke(p, rc, NULL);
                }
            } else {
                zrpc_pending_invoke(p, ZRPC_ERR_NOMEM, NULL);
            }
        } else {
            zrpc_pending_invoke(p, rc, NULL);
        }
    } else {
        node->metrics.requests_sent++;
        node->metrics.bytes_sent += len;
    }
}

static void call_task_fn(void *user) {
    zrpc_call_task_t *t = (zrpc_call_task_t *)user;
    call_now(t->proxy->node, t->proxy, t->id, t->service, t->method, t->data, t->len, t->encoding,
             t->cb, t->chunk_cb, t->done_cb, t->streaming, t->user, t->timeout_ms);
    if (t->data) {
        free(t->data);
    }
    free(t);
}

static void cancel_task_fn(void *user) {
    zrpc_cancel_task_t *t = (zrpc_cancel_task_t *)user;
    zrpc_pending_t *p = zrpc_pending_find(t->proxy->node, t->id);
    if (p) {
        zrpc_pending_invoke(p, ZRPC_ERR_NOTFOUND, NULL);
    }
    free(t);
}

int zrpc_proxy_create(zrpc_node_t *node, const zrpc_proxy_options_t *options, zrpc_proxy_t **out) {
    zrpc_proxy_t *p;
    if (!node || !out) {
        return ZRPC_ERR_INVALID;
    }
    *out = NULL;
    p = (zrpc_proxy_t *)calloc(1, sizeof(*p));
    if (!p) {
        return ZRPC_ERR_NOMEM;
    }
    p->node = node;
    p->payload_encoding = options ? options->payload_encoding : 0;
    p->prefer_stream_threshold = options ? options->prefer_stream_threshold : 0;
    *out = p;
    return ZRPC_OK;
}

void zrpc_proxy_destroy(zrpc_proxy_t *proxy) {
    free(proxy);
}

int zrpc_proxy_set_endpoint(zrpc_proxy_t *proxy, const char *ip, uint16_t port,
                            zrpc_transport_kind_t transport) {
    if (!proxy || !ip || !port) {
        return ZRPC_ERR_INVALID;
    }
    zrpc_copy_str(proxy->endpoint_ip, sizeof(proxy->endpoint_ip), ip, "127.0.0.1");
    proxy->endpoint_port = port;
    proxy->endpoint_transport = transport;
    zrpc_copy_str(proxy->endpoint_scheme, sizeof(proxy->endpoint_scheme),
                  transport == ZRPC_TRANSPORT_TCP ? "tcp" : "udp", "udp");
    proxy->has_endpoint = 1;
    return ZRPC_OK;
}

int zrpc_proxy_set_endpoint_uri(zrpc_proxy_t *proxy, const char *endpoint) {
    zrpc_endpoint_t parsed;
    if (!proxy || !endpoint || zrpc_endpoint_parse(endpoint, &parsed) != ZRPC_OK ||
        parsed.port == 0) {
        return ZRPC_ERR_INVALID;
    }
    zrpc_copy_str(proxy->endpoint_ip, sizeof(proxy->endpoint_ip), parsed.host, "127.0.0.1");
    zrpc_copy_str(proxy->endpoint_scheme, sizeof(proxy->endpoint_scheme), parsed.scheme, "tcp");
    proxy->endpoint_port = parsed.port;
    proxy->endpoint_transport = parsed.transport;
    proxy->has_endpoint = 1;
    return ZRPC_OK;
}

int zrpc_proxy_stream_open(zrpc_proxy_t *proxy, const char *service, const char *method,
                           zrpc_response_fn cb, void *user, uint64_t timeout_ms,
                           zrpc_stream_t **out_stream) {
    zrpc_stream_t *stream;
    zrpc_stream_open_task_t *task;
    zrpc_transport_kind_t transport;
    if (!proxy || !proxy->node || !service || !method || !cb || !out_stream) {
        return ZRPC_ERR_INVALID;
    }
    *out_stream = NULL;
    stream = (zrpc_stream_t *)calloc(1, sizeof(*stream));
    if (!stream) {
        return ZRPC_ERR_NOMEM;
    }
    stream->proxy = proxy;
    stream->node = proxy->node;
    stream->cb = cb;
    stream->user = user;
    stream->timeout_ms = timeout_ms;
    zrpc_copy_str(stream->service, sizeof(stream->service), service, "");
    zrpc_copy_str(stream->method, sizeof(stream->method), method, "");
    zrpc_copy_str(stream->scheme, sizeof(stream->scheme), "udp", "udp");
    zrpc_copy_str(stream->ip, sizeof(stream->ip), "127.0.0.1", "127.0.0.1");
    ztk_mutex_lock(proxy->node->id_lock);
    stream->request_id = (uint64_t)(uint32_t)(proxy->node->next_request_id++);
    ztk_mutex_unlock(proxy->node->id_lock);
    if (stream->request_id == 0) {
        stream->request_id = 1;
    }
    stream->stream_id = (uint32_t)stream->request_id;
    if (zrpc_proxy_resolve(proxy, service, stream->ip, sizeof(stream->ip), &stream->port,
                           &transport, stream->scheme, sizeof(stream->scheme), 0) != ZRPC_OK) {
        free(stream);
        return ZRPC_ERR_NOTFOUND;
    }
    stream->transport = transport;
    task = (zrpc_stream_open_task_t *)calloc(1, sizeof(*task));
    if (!task) {
        free(stream);
        return ZRPC_ERR_NOMEM;
    }
    task->stream = stream;
    if (ztk_poller_async(proxy->node->poller, stream_open_task_fn, task, 1) != ZTK_OK) {
        free(task);
        free(stream);
        return ZRPC_ERR_IO;
    }
    *out_stream = stream;
    return ZRPC_OK;
}

int zrpc_proxy_stream_open_bidi(zrpc_proxy_t *proxy, const char *service, const char *method,
                                zrpc_response_chunk_fn on_chunk, zrpc_stream_done_fn on_done,
                                void *user, uint64_t timeout_ms, zrpc_stream_t **out_stream) {
    zrpc_stream_t *stream;
    zrpc_stream_open_task_t *task;
    zrpc_transport_kind_t transport;
    if (!proxy || !proxy->node || !service || !method || !on_chunk || !on_done || !out_stream) {
        return ZRPC_ERR_INVALID;
    }
    *out_stream = NULL;
    stream = (zrpc_stream_t *)calloc(1, sizeof(*stream));
    if (!stream) {
        return ZRPC_ERR_NOMEM;
    }
    stream->proxy = proxy;
    stream->node = proxy->node;
    stream->bidi = 1;
    stream->chunk_cb = on_chunk;
    stream->done_cb = on_done;
    stream->user = user;
    stream->timeout_ms = timeout_ms;
    zrpc_copy_str(stream->service, sizeof(stream->service), service, "");
    zrpc_copy_str(stream->method, sizeof(stream->method), method, "");
    zrpc_copy_str(stream->scheme, sizeof(stream->scheme), "udp", "udp");
    zrpc_copy_str(stream->ip, sizeof(stream->ip), "127.0.0.1", "127.0.0.1");
    ztk_mutex_lock(proxy->node->id_lock);
    stream->request_id = (uint64_t)(uint32_t)(proxy->node->next_request_id++);
    ztk_mutex_unlock(proxy->node->id_lock);
    if (stream->request_id == 0) {
        stream->request_id = 1;
    }
    stream->stream_id = (uint32_t)stream->request_id;
    if (zrpc_proxy_resolve(proxy, service, stream->ip, sizeof(stream->ip), &stream->port,
                           &transport, stream->scheme, sizeof(stream->scheme), 0) != ZRPC_OK) {
        free(stream);
        return ZRPC_ERR_NOTFOUND;
    }
    stream->transport = transport;
    task = (zrpc_stream_open_task_t *)calloc(1, sizeof(*task));
    if (!task) {
        free(stream);
        return ZRPC_ERR_NOMEM;
    }
    task->stream = stream;
    if (ztk_poller_async(proxy->node->poller, stream_open_task_fn, task, 1) != ZTK_OK) {
        free(task);
        free(stream);
        return ZRPC_ERR_IO;
    }
    *out_stream = stream;
    return ZRPC_OK;
}

int zrpc_proxy_stream_send(zrpc_stream_t *stream, const zrpc_chunk_t *chunk) {
    zrpc_stream_send_task_t *task;
    int first;
    if (!stream || !chunk || (!chunk->data && chunk->len) || stream->closed || stream->send_closed ||
        (!stream->sent_any && !(chunk->flags & ZRPC_STREAM_FIRST)) ||
        (stream->sent_any && (chunk->flags & ZRPC_STREAM_FIRST))) {
        return ZRPC_ERR_STATE;
    }
    if (stream->sent_any && chunk->offset != stream->next_offset) {
        return ZRPC_ERR_STATE;
    }
    if (!stream->sent_any && chunk->offset != 0) {
        return ZRPC_ERR_STATE;
    }
    first = !stream->sent_any;
    stream->sent_any = 1;
    stream->next_offset = chunk->offset + chunk->len;
    if (chunk->flags & ZRPC_STREAM_LAST) {
        stream->send_closed = 1;
    }
    task = (zrpc_stream_send_task_t *)calloc(1, sizeof(*task));
    if (!task) {
        return ZRPC_ERR_NOMEM;
    }
    task->stream = stream;
    task->len = chunk->len;
    task->encoding = chunk->encoding;
    task->offset = chunk->offset;
    task->flags = chunk->flags;
    task->first = first;
    if (chunk->len > 0) {
        task->data = (uint8_t *)malloc(chunk->len);
        if (!task->data) {
            free(task);
            return ZRPC_ERR_NOMEM;
        }
        memcpy(task->data, chunk->data, chunk->len);
    }
    if (ztk_poller_async(stream->node->poller, stream_send_async_fn, task, 1) != ZTK_OK) {
        free(task->data);
        free(task);
        return ZRPC_ERR_IO;
    }
    return ZRPC_OK;
}

int zrpc_proxy_stream_cancel(zrpc_stream_t *stream) {
    if (!stream || stream->closed) {
        return ZRPC_ERR_NOTFOUND;
    }
    if (ztk_poller_is_current_thread(stream->node->poller)) {
        stream_cancel_task_fn(stream);
        return ZRPC_OK;
    }
    if (ztk_poller_async(stream->node->poller, stream_cancel_task_fn, stream, 1) != ZTK_OK) {
        return ZRPC_ERR_IO;
    }
    return ZRPC_OK;
}

int zrpc_proxy_stream_destroy(zrpc_stream_t *stream) {
    if (!stream) {
        return ZRPC_ERR_INVALID;
    }
    if (!stream->closed) {
        return ZRPC_ERR_STATE;
    }
    free(stream);
    return ZRPC_OK;
}

int zrpc_proxy_call(zrpc_proxy_t *proxy, const char *service, const char *method,
                    const zrpc_payload_t *payload, zrpc_response_fn cb, void *user,
                    uint64_t timeout_ms, uint64_t *out_request_id) {
    zrpc_call_task_t *t;
    uint64_t id;
    const uint8_t *data = payload ? (const uint8_t *)payload->data : NULL;
    size_t len = payload ? payload->len : 0;
    uint8_t encoding = payload ? payload->encoding : 0;

    if (!proxy || !proxy->node || !service || !method || !cb) {
        return ZRPC_ERR_INVALID;
    }
    if (len > proxy->node->max_msg_bytes || (len > 0 && !data)) {
        return ZRPC_ERR_INVALID;
    }
    if (encoding == 0) {
        encoding = proxy->payload_encoding;
    }

    ztk_mutex_lock(proxy->node->id_lock);
    id = (uint64_t)(uint32_t)(proxy->node->next_request_id++);
    ztk_mutex_unlock(proxy->node->id_lock);
    if (id == 0) {
        id = 1;
    }

    /* 已在 poller 线程：内联发送，避免跨线程投递 */
    if (ztk_poller_is_current_thread(proxy->node->poller)) {
        call_now(proxy->node, proxy, id, service, method, data, len, encoding, cb, NULL, NULL, 0,
                 user, timeout_ms);
        if (out_request_id) {
            *out_request_id = id;
        }
        return ZRPC_OK;
    }

    t = (zrpc_call_task_t *)calloc(1, sizeof(*t));
    if (!t) {
        return ZRPC_ERR_NOMEM;
    }
    t->proxy = proxy;
    t->id = id;
    t->timeout_ms = timeout_ms;
    t->cb = cb;
    t->user = user;
    zrpc_copy_str(t->service, sizeof(t->service), service, "");
    zrpc_copy_str(t->method, sizeof(t->method), method, "");
    t->len = len;
    t->encoding = encoding;
    if (len > 0) {
        t->data = (uint8_t *)malloc(len);
        if (!t->data) {
            free(t);
            return ZRPC_ERR_NOMEM;
        }
        memcpy(t->data, data, len);
    }
    if (ztk_poller_async(proxy->node->poller, call_task_fn, t, 1) != ZTK_OK) {
        if (t->data) {
            free(t->data);
        }
        free(t);
        return ZRPC_ERR_IO;
    }
    if (out_request_id) {
        *out_request_id = id;
    }
    return ZRPC_OK;
}

int zrpc_proxy_call_stream(zrpc_proxy_t *proxy, const char *service, const char *method,
                           const zrpc_payload_t *request, zrpc_response_chunk_fn on_chunk,
                           zrpc_stream_done_fn on_done, void *user, uint64_t timeout_ms,
                           uint64_t *out_request_id) {
    zrpc_call_task_t *task;
    uint64_t id;
    const uint8_t *data = request ? (const uint8_t *)request->data : NULL;
    size_t len = request ? request->len : 0;
    uint8_t encoding = request ? request->encoding : 0;
    if (!proxy || !proxy->node || !service || !method || !on_chunk || !on_done) {
        return ZRPC_ERR_INVALID;
    }
    if (len > proxy->node->max_msg_bytes || (len > 0 && !data)) {
        return ZRPC_ERR_INVALID;
    }
    if (encoding == 0) {
        encoding = proxy->payload_encoding;
    }
    ztk_mutex_lock(proxy->node->id_lock);
    id = (uint64_t)(uint32_t)(proxy->node->next_request_id++);
    ztk_mutex_unlock(proxy->node->id_lock);
    if (id == 0) {
        id = 1;
    }
    if (ztk_poller_is_current_thread(proxy->node->poller)) {
        call_now(proxy->node, proxy, id, service, method, data, len, encoding, NULL, on_chunk,
                 on_done, 1, user, timeout_ms);
        if (out_request_id) {
            *out_request_id = id;
        }
        return ZRPC_OK;
    }
    task = (zrpc_call_task_t *)calloc(1, sizeof(*task));
    if (!task) {
        return ZRPC_ERR_NOMEM;
    }
    task->proxy = proxy;
    task->id = id;
    task->timeout_ms = timeout_ms;
    task->chunk_cb = on_chunk;
    task->done_cb = on_done;
    task->streaming = 1;
    task->user = user;
    zrpc_copy_str(task->service, sizeof(task->service), service, "");
    zrpc_copy_str(task->method, sizeof(task->method), method, "");
    task->len = len;
    task->encoding = encoding;
    if (len > 0) {
        task->data = (uint8_t *)malloc(len);
        if (!task->data) {
            free(task);
            return ZRPC_ERR_NOMEM;
        }
        memcpy(task->data, data, len);
    }
    if (ztk_poller_async(proxy->node->poller, call_task_fn, task, 1) != ZTK_OK) {
        free(task->data);
        free(task);
        return ZRPC_ERR_IO;
    }
    if (out_request_id) {
        *out_request_id = id;
    }
    return ZRPC_OK;
}

int zrpc_proxy_cancel(zrpc_proxy_t *proxy, uint64_t request_id) {
    zrpc_cancel_task_t *t;
    if (!proxy || !proxy->node || !request_id) {
        return ZRPC_ERR_INVALID;
    }
    t = (zrpc_cancel_task_t *)calloc(1, sizeof(*t));
    if (!t) {
        return ZRPC_ERR_NOMEM;
    }
    t->proxy = proxy;
    t->id = request_id;
    if (ztk_poller_async(proxy->node->poller, cancel_task_fn, t, 1) != ZTK_OK) {
        free(t);
        return ZRPC_ERR_IO;
    }
    return ZRPC_OK;
}

/* RTO 重传：按保存的请求参数重发（同一 msg_id）。 */
void zrpc_pending_retx(zrpc_node_t *node, zrpc_pending_t *p) {
    char route[ZRPC_SERVICE_NAME_MAX + ZRPC_METHOD_NAME_MAX + 2];
    zrpc_payload_t pl;
    if (!node || !p || !p->has_req) {
        return;
    }
    snprintf(route, sizeof(route), "%s.%s", p->service, p->method);
    pl.data = p->data;
    pl.len = p->len;
    pl.encoding = p->encoding;
    (void)zrpc_node_send(node, p->ip, p->port, ZRPC_KIND_REQUEST, (uint32_t)p->request_id,
                         p->transport, p->scheme, route, &pl, NULL);
    node->metrics.retries++;
}
