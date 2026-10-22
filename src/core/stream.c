/*
 * 客户端流式会话：分片上传 / 双向流。
 *
 * 从 proxy.c 拆出，只保留流式相关的任务与生命周期；与服务端
 * zri_service_handle_stream_chunk 对称。
 */
#include "proxy_internal.h"

#include "envelope.h"
#include "node_internal.h"
#include "pending.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

typedef struct zri_stream_open_task {
    stream_t *stream;
} zri_stream_open_task_t;

typedef struct zri_stream_send_task {
    stream_t *stream;
    uint8_t *data;
    size_t len;
    uint8_t encoding;
    uint64_t offset;
    unsigned flags;
    int first;
    int retries;
} zri_stream_send_task_t;

static uint64_t stream_send_task_fn(void *user);

static void stream_send_async_fn(void *user)
{
    (void)stream_send_task_fn(user);
}

static void stream_response(int status, const response_t *resp, void *user)
{
    stream_t *stream = (stream_t *)user;
    if (!stream) {
        return;
    }
    stream->closed = 1;
    if (stream->cb) {
        stream->cb(status, resp, stream->user);
    }
}

static void stream_bidi_chunk(int status, const chunk_t *chunk, void *user)
{
    stream_t *stream = (stream_t *)user;
    if (stream && stream->chunk_cb) {
        stream->chunk_cb(status, chunk, stream->user);
    }
}

static void stream_bidi_done(int status, void *user)
{
    stream_t *stream = (stream_t *)user;
    if (!stream) {
        return;
    }
    stream->closed = 1;
    if (stream->done_cb) {
        stream->done_cb(status, stream->user);
    }
}

static void stream_open_task_fn(void *user)
{
    zri_stream_open_task_t *task = (zri_stream_open_task_t *)user;
    stream_t *stream = task->stream;
    zri_pending_t *pending;
    int prc = OK;
    if (stream->closed) {
        free(task);
        return;
    }
    pending = zri_pending_add(stream->node,
                              stream->request_id,
                              stream->bidi ? NULL : stream_response,
                              stream,
                              stream->timeout_ms,
                              stream->node->poller,
                              &prc);
    if (!pending) {
        stream_response(prc == OK ? ERR_NOMEM : prc, NULL, stream);
    } else {
        pending->owner = stream->stub;
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

static uint64_t stream_send_task_fn(void *user)
{
    zri_stream_send_task_t *task = (zri_stream_send_task_t *)user;
    stream_t *stream = task->stream;
    payload_t payload;
    stream_meta_t meta;
    char route[MAX_ROUTE];
    zri_message_t message;
    int rc;
    if (stream->closed) {
        free(task->data);
        free(task);
        return 0;
    }
    if (task->first) {
        if (zri_route_format(route, sizeof(route), stream->service, stream->method) != OK) {
            rc = ERR_INVALID;
            goto done;
        }
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
    memset(&message, 0, sizeof(message));
    message.kind = KIND_REQUEST;
    message.request_id = (uint32_t)stream->request_id;
    message.route = route;
    message.payload = payload;
    message.stream = meta;
    message.has_stream = 1;
    message.peer_ip = stream->ip;
    message.peer_port = stream->port;
    rc = zri_node_send(stream->node, stream->transport, stream->scheme, &message);
    if (rc == ERR_AGAIN && task->retries++ < 1000) {
        if (ntk_poller_do_delay(stream->node->poller, 1, stream_send_task_fn, task) != NULL) {
            return 0; /* 保留 task/data，稍后重试 */
        }
    }
done:
    if (rc != OK) {
        zri_pending_t *pending = zri_pending_find(stream->node, stream->request_id);
        if (pending) {
            zri_pending_invoke(pending, rc, NULL);
        }
    }
    free(task->data);
    free(task);
    return 0;
}

static void stream_cancel_task_fn(void *user)
{
    stream_t *stream = (stream_t *)user;
    zri_pending_t *pending;
    if (!stream || stream->closed) {
        return;
    }
    {
        payload_t empty;
        stream_meta_t meta;
        zri_message_t message;
        char route[MAX_ROUTE];
        memset(&empty, 0, sizeof(empty));
        memset(&meta, 0, sizeof(meta));
        meta.flags = STREAM_CANCEL;
        meta.stream_id = stream->stream_id;
        if (zri_route_format(route, sizeof(route), stream->service, stream->method) == OK) {
            memset(&message, 0, sizeof(message));
            message.kind = KIND_REQUEST;
            message.request_id = (uint32_t)stream->request_id;
            message.route = route;
            message.payload = empty;
            message.stream = meta;
            message.has_stream = 1;
            message.peer_ip = stream->ip;
            message.peer_port = stream->port;
            (void)zri_node_send(stream->node, stream->transport, stream->scheme, &message);
        }
    }
    pending = zri_pending_find(stream->node, stream->request_id);
    if (pending) {
        zri_pending_invoke(pending, ERR_CLOSED, NULL);
    } else {
        stream->closed = 1;
    }
}

/* ---- 流打开 ---- */

int proxy_stream_open(proxy_t *proxy,
                      const char *service,
                      const char *method,
                      response_fn cb,
                      void *user,
                      uint64_t timeout_ms,
                      stream_t **out_stream)
{
    stream_t *stream;
    zri_stream_open_task_t *task;
    transport_kind_t transport;
    if (!proxy || !proxy->node || !service || !method || !cb || !out_stream) {
        return ERR_INVALID;
    }
    *out_stream = NULL;
    stream = (stream_t *)calloc(1, sizeof(*stream));
    if (!stream) {
        return ERR_NOMEM;
    }
    stream->proxy = proxy;
    stream->stub = proxy->stub;
    stream->node = proxy->node;
    stream->cb = cb;
    stream->user = user;
    stream->timeout_ms = timeout_ms;
    zri_copy_str(stream->service, sizeof(stream->service), service, "");
    zri_copy_str(stream->method, sizeof(stream->method), method, "");
    zri_copy_str(stream->scheme, sizeof(stream->scheme), "udp", "udp");
    zri_copy_str(stream->ip, sizeof(stream->ip), "127.0.0.1", "127.0.0.1");
    ntk_mutex_lock(proxy->node->id_lock);
    stream->request_id = (uint64_t)(uint32_t)(proxy->node->next_request_id++);
    ntk_mutex_unlock(proxy->node->id_lock);
    if (stream->request_id == 0) {
        stream->request_id = 1;
    }
    stream->stream_id = (uint32_t)stream->request_id;
    if (zri_proxy_resolve(proxy,
                          service,
                          stream->ip,
                          sizeof(stream->ip),
                          &stream->port,
                          &transport,
                          stream->scheme,
                          sizeof(stream->scheme),
                          0) != OK) {
        free(stream);
        return ERR_NOTFOUND;
    }
    stream->transport = transport;
    task = (zri_stream_open_task_t *)calloc(1, sizeof(*task));
    if (!task) {
        free(stream);
        return ERR_NOMEM;
    }
    task->stream = stream;
    if (ntk_poller_async(proxy->node->poller, stream_open_task_fn, task, 1) != NTK_OK) {
        free(task);
        free(stream);
        return ERR_IO;
    }
    *out_stream = stream;
    return OK;
}

int proxy_stream_open_bidi(proxy_t *proxy,
                           const char *service,
                           const char *method,
                           response_chunk_fn on_chunk,
                           stream_done_fn on_done,
                           void *user,
                           uint64_t timeout_ms,
                           stream_t **out_stream)
{
    stream_t *stream;
    zri_stream_open_task_t *task;
    transport_kind_t transport;
    if (!proxy || !proxy->node || !service || !method || !on_chunk || !on_done || !out_stream) {
        return ERR_INVALID;
    }
    *out_stream = NULL;
    stream = (stream_t *)calloc(1, sizeof(*stream));
    if (!stream) {
        return ERR_NOMEM;
    }
    stream->proxy = proxy;
    stream->stub = proxy->stub;
    stream->node = proxy->node;
    stream->bidi = 1;
    stream->chunk_cb = on_chunk;
    stream->done_cb = on_done;
    stream->user = user;
    stream->timeout_ms = timeout_ms;
    zri_copy_str(stream->service, sizeof(stream->service), service, "");
    zri_copy_str(stream->method, sizeof(stream->method), method, "");
    zri_copy_str(stream->scheme, sizeof(stream->scheme), "udp", "udp");
    zri_copy_str(stream->ip, sizeof(stream->ip), "127.0.0.1", "127.0.0.1");
    ntk_mutex_lock(proxy->node->id_lock);
    stream->request_id = (uint64_t)(uint32_t)(proxy->node->next_request_id++);
    ntk_mutex_unlock(proxy->node->id_lock);
    if (stream->request_id == 0) {
        stream->request_id = 1;
    }
    stream->stream_id = (uint32_t)stream->request_id;
    if (zri_proxy_resolve(proxy,
                          service,
                          stream->ip,
                          sizeof(stream->ip),
                          &stream->port,
                          &transport,
                          stream->scheme,
                          sizeof(stream->scheme),
                          0) != OK) {
        free(stream);
        return ERR_NOTFOUND;
    }
    stream->transport = transport;
    task = (zri_stream_open_task_t *)calloc(1, sizeof(*task));
    if (!task) {
        free(stream);
        return ERR_NOMEM;
    }
    task->stream = stream;
    if (ntk_poller_async(proxy->node->poller, stream_open_task_fn, task, 1) != NTK_OK) {
        free(task);
        free(stream);
        return ERR_IO;
    }
    *out_stream = stream;
    return OK;
}

/* ---- 流数据与生命周期 ---- */

int proxy_stream_send(stream_t *stream, const chunk_t *chunk)
{
    zri_stream_send_task_t *task;
    int first;
    if (!stream || !chunk || (!chunk->data && chunk->len) || stream->closed ||
        stream->send_closed || (!stream->sent_any && !(chunk->flags & STREAM_FIRST)) ||
        (stream->sent_any && (chunk->flags & STREAM_FIRST))) {
        return ERR_STATE;
    }
    if (stream->sent_any && chunk->offset != stream->next_offset) {
        return ERR_STATE;
    }
    if (!stream->sent_any && chunk->offset != 0) {
        return ERR_STATE;
    }
    first = !stream->sent_any;
    stream->sent_any = 1;
    stream->next_offset = chunk->offset + chunk->len;
    if (chunk->flags & STREAM_LAST) {
        stream->send_closed = 1;
    }
    task = (zri_stream_send_task_t *)calloc(1, sizeof(*task));
    if (!task) {
        return ERR_NOMEM;
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
            return ERR_NOMEM;
        }
        memcpy(task->data, chunk->data, chunk->len);
    }
    if (ntk_poller_async(stream->node->poller, stream_send_async_fn, task, 1) != NTK_OK) {
        free(task->data);
        free(task);
        return ERR_IO;
    }
    return OK;
}

int proxy_stream_cancel(stream_t *stream)
{
    if (!stream || stream->closed) {
        return ERR_NOTFOUND;
    }
    if (ntk_poller_is_current_thread(stream->node->poller)) {
        stream_cancel_task_fn(stream);
        return OK;
    }
    if (ntk_poller_async(stream->node->poller, stream_cancel_task_fn, stream, 1) != NTK_OK) {
        return ERR_IO;
    }
    return OK;
}

int proxy_stream_destroy(stream_t *stream)
{
    if (!stream) {
        return ERR_INVALID;
    }
    if (!stream->closed) {
        return ERR_STATE;
    }
    free(stream);
    return OK;
}
