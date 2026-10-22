/*
 * 客户端流式会话：分片上传 / 双向流。
 *
 * 从 proxy.c 拆出，只保留流式相关的任务与生命周期；与服务端
 * nb_service_handle_stream_chunk 对称。
 */
#include "proxy_internal.h"

#include "envelope.h"
#include "node_internal.h"
#include "pending.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

typedef struct nb_stream_open_task {
    nb_stream_t *stream;
} nb_stream_open_task_t;

typedef struct nb_stream_send_task {
    nb_stream_t *stream;
    uint8_t *data;
    size_t len;
    uint8_t encoding;
    uint8_t metadata[NB_METADATA_WIRE_MAX];
    uint16_t metadata_len;
    char message[NB_STATUS_MSG_MAX];
    uint16_t message_len;
    uint64_t offset;
    unsigned flags;
    int first;
    int retries;
} nb_stream_send_task_t;

static uint64_t nb_stream_send_task_fn(void *user);

static void nb_stream_send_async_fn(void *user)
{
    (void)nb_stream_send_task_fn(user);
}

static void nb_stream_response(int status, const nb_response_t *resp, void *user)
{
    nb_stream_t *stream = (nb_stream_t *)user;
    if (!stream) {
        return;
    }
    stream->closed = 1;
    if (stream->cb) {
        stream->cb(status, resp, stream->user);
    }
}

static void nb_stream_bidi_chunk(int status, const nb_chunk_t *chunk, void *user)
{
    nb_stream_t *stream = (nb_stream_t *)user;
    if (stream && stream->chunk_cb) {
        stream->chunk_cb(status, chunk, stream->user);
    }
}

static void nb_stream_bidi_done(int status, void *user)
{
    nb_stream_t *stream = (nb_stream_t *)user;
    if (!stream) {
        return;
    }
    stream->closed = 1;
    if (stream->done_cb) {
        stream->done_cb(status, stream->user);
    }
}

static void nb_stream_open_task_fn(void *user)
{
    nb_stream_open_task_t *task = (nb_stream_open_task_t *)user;
    nb_stream_t *stream = task->stream;
    nb_pending_t *pending;
    int prc = NB_OK;
    if (stream->closed) {
        free(task);
        return;
    }
    pending = nb_pending_add(stream->node,
                              stream->request_id,
                              stream->bidi ? NULL : nb_stream_response,
                              stream,
                              stream->timeout_ms,
                              stream->node->poller,
                              &prc);
    if (!pending) {
        nb_stream_response(prc == NB_OK ? NB_ERR_NOMEM : prc, NULL, stream);
    } else {
        pending->owner = stream->stub;
        if (stream->bidi) {
            pending->streaming = 1;
            pending->chunk_cb = nb_stream_bidi_chunk;
            pending->done_cb = nb_stream_bidi_done;
            pending->stream_user = stream;
        }
        stream->opened = 1;
    }
    free(task);
}

static uint64_t nb_stream_send_task_fn(void *user)
{
    nb_stream_send_task_t *task = (nb_stream_send_task_t *)user;
    nb_stream_t *stream = task->stream;
    nb_payload_t payload;
    nb_stream_meta_t meta;
    char route[MAX_ROUTE];
    nb_message_t message;
    int rc;
    if (stream->closed) {
        free(task->data);
        free(task);
        return 0;
    }
    if (task->first) {
        if (nb_route_format(route, sizeof(route), stream->service, stream->method) != NB_OK) {
            rc = NB_ERR_INVALID;
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
    message.metadata = task->metadata_len ? task->metadata : NULL;
    message.metadata_len = task->metadata_len;
    message.status_msg = task->message_len ? task->message : NULL;
    message.status_msg_len = task->message_len;
    message.route = route;
    message.payload = payload;
    message.stream = meta;
    message.has_stream = 1;
    message.peer_ip = stream->ip;
    message.peer_port = stream->port;
    rc = nb_node_send(stream->node, stream->transport, stream->scheme, &message);
    if (rc == NB_ERR_AGAIN && task->retries++ < 1000) {
        if (ntk_poller_do_delay(stream->node->poller, 1, nb_stream_send_task_fn, task) != NULL) {
            return 0; /* 保留 task/data，稍后重试 */
        }
    }
done:
    if (rc != NB_OK) {
        nb_pending_t *pending = nb_pending_find(stream->node, stream->request_id);
        if (pending) {
            nb_pending_invoke(pending, rc, NULL);
        }
    }
    free(task->data);
    free(task);
    return 0;
}

static void nb_stream_cancel_task_fn(void *user)
{
    nb_stream_t *stream = (nb_stream_t *)user;
    nb_pending_t *pending;
    if (!stream || stream->closed) {
        return;
    }
    {
        nb_payload_t empty;
        nb_stream_meta_t meta;
        nb_message_t message;
        char route[MAX_ROUTE];
        memset(&empty, 0, sizeof(empty));
        memset(&meta, 0, sizeof(meta));
        meta.flags = NB_STREAM_CANCEL;
        meta.stream_id = stream->stream_id;
        if (nb_route_format(route, sizeof(route), stream->service, stream->method) == NB_OK) {
            memset(&message, 0, sizeof(message));
            message.kind = KIND_REQUEST;
            message.request_id = (uint32_t)stream->request_id;
            message.route = route;
            message.payload = empty;
            message.stream = meta;
            message.has_stream = 1;
            message.peer_ip = stream->ip;
            message.peer_port = stream->port;
            (void)nb_node_send(stream->node, stream->transport, stream->scheme, &message);
        }
    }
    pending = nb_pending_find(stream->node, stream->request_id);
    if (pending) {
        nb_pending_invoke(pending, NB_ERR_CLOSED, NULL);
    } else {
        stream->closed = 1;
    }
}

/* ---- 流打开 ---- */

int nb_proxy_stream_open(nb_proxy_t *proxy,
                      const char *service,
                      const char *method,
                      response_fn cb,
                      void *user,
                      uint64_t timeout_ms,
                      nb_stream_t **out_stream)
{
    nb_stream_t *stream;
    nb_stream_open_task_t *task;
    nb_transport_kind_t transport;
    if (!proxy || !proxy->node || !service || !method || !cb || !out_stream) {
        return NB_ERR_INVALID;
    }
    *out_stream = NULL;
    stream = (nb_stream_t *)calloc(1, sizeof(*stream));
    if (!stream) {
        return NB_ERR_NOMEM;
    }
    stream->proxy = proxy;
    stream->stub = proxy->stub;
    stream->node = proxy->node;
    stream->cb = cb;
    stream->user = user;
    stream->timeout_ms = timeout_ms;
    nb_copy_str(stream->service, sizeof(stream->service), service, "");
    nb_copy_str(stream->method, sizeof(stream->method), method, "");
    nb_copy_str(stream->scheme, sizeof(stream->scheme), "udp", "udp");
    nb_copy_str(stream->ip, sizeof(stream->ip), "127.0.0.1", "127.0.0.1");
    ntk_mutex_lock(proxy->node->id_lock);
    stream->request_id = (uint64_t)(uint32_t)(proxy->node->next_request_id++);
    ntk_mutex_unlock(proxy->node->id_lock);
    if (stream->request_id == 0) {
        stream->request_id = 1;
    }
    stream->stream_id = (uint32_t)stream->request_id;
    if (nb_proxy_resolve(proxy,
                          service,
                          stream->ip,
                          sizeof(stream->ip),
                          &stream->port,
                          &transport,
                          stream->scheme,
                          sizeof(stream->scheme),
                          0) != NB_OK) {
        free(stream);
        return NB_ERR_NOTFOUND;
    }
    stream->transport = transport;
    task = (nb_stream_open_task_t *)calloc(1, sizeof(*task));
    if (!task) {
        free(stream);
        return NB_ERR_NOMEM;
    }
    task->stream = stream;
    if (ntk_poller_async(proxy->node->poller, nb_stream_open_task_fn, task, 1) != NTK_OK) {
        free(task);
        free(stream);
        return NB_ERR_IO;
    }
    *out_stream = stream;
    return NB_OK;
}

int nb_proxy_stream_open_bidi(nb_proxy_t *proxy,
                           const char *service,
                           const char *method,
                           response_chunk_fn on_chunk,
                           stream_done_fn on_done,
                           void *user,
                           uint64_t timeout_ms,
                           nb_stream_t **out_stream)
{
    nb_stream_t *stream;
    nb_stream_open_task_t *task;
    nb_transport_kind_t transport;
    if (!proxy || !proxy->node || !service || !method || !on_chunk || !on_done || !out_stream) {
        return NB_ERR_INVALID;
    }
    *out_stream = NULL;
    stream = (nb_stream_t *)calloc(1, sizeof(*stream));
    if (!stream) {
        return NB_ERR_NOMEM;
    }
    stream->proxy = proxy;
    stream->stub = proxy->stub;
    stream->node = proxy->node;
    stream->bidi = 1;
    stream->chunk_cb = on_chunk;
    stream->done_cb = on_done;
    stream->user = user;
    stream->timeout_ms = timeout_ms;
    nb_copy_str(stream->service, sizeof(stream->service), service, "");
    nb_copy_str(stream->method, sizeof(stream->method), method, "");
    nb_copy_str(stream->scheme, sizeof(stream->scheme), "udp", "udp");
    nb_copy_str(stream->ip, sizeof(stream->ip), "127.0.0.1", "127.0.0.1");
    ntk_mutex_lock(proxy->node->id_lock);
    stream->request_id = (uint64_t)(uint32_t)(proxy->node->next_request_id++);
    ntk_mutex_unlock(proxy->node->id_lock);
    if (stream->request_id == 0) {
        stream->request_id = 1;
    }
    stream->stream_id = (uint32_t)stream->request_id;
    if (nb_proxy_resolve(proxy,
                          service,
                          stream->ip,
                          sizeof(stream->ip),
                          &stream->port,
                          &transport,
                          stream->scheme,
                          sizeof(stream->scheme),
                          0) != NB_OK) {
        free(stream);
        return NB_ERR_NOTFOUND;
    }
    stream->transport = transport;
    task = (nb_stream_open_task_t *)calloc(1, sizeof(*task));
    if (!task) {
        free(stream);
        return NB_ERR_NOMEM;
    }
    task->stream = stream;
    if (ntk_poller_async(proxy->node->poller, nb_stream_open_task_fn, task, 1) != NTK_OK) {
        free(task);
        free(stream);
        return NB_ERR_IO;
    }
    *out_stream = stream;
    return NB_OK;
}

/* ---- 流数据与生命周期 ---- */

int nb_proxy_stream_send(nb_stream_t *stream, const nb_chunk_t *chunk)
{
    nb_stream_send_task_t *task;
    int first;
    if (!stream || !chunk || (!chunk->data && chunk->len) || stream->closed ||
        stream->send_closed || (!stream->sent_any && !(chunk->flags & NB_STREAM_FIRST)) ||
        (stream->sent_any && (chunk->flags & NB_STREAM_FIRST))) {
        return NB_ERR_STATE;
    }
    if (stream->sent_any && chunk->offset != stream->next_offset) {
        return NB_ERR_STATE;
    }
    if (!stream->sent_any && chunk->offset != 0) {
        return NB_ERR_STATE;
    }
    first = !stream->sent_any;
    stream->sent_any = 1;
    stream->next_offset = chunk->offset + chunk->len;
    if (chunk->flags & NB_STREAM_LAST) {
        stream->send_closed = 1;
    }
    task = (nb_stream_send_task_t *)calloc(1, sizeof(*task));
    if (!task) {
        return NB_ERR_NOMEM;
    }
    task->stream = stream;
    task->len = chunk->len;
    task->encoding = chunk->encoding;
    task->offset = chunk->offset;
    task->flags = chunk->flags;
    task->first = first;
    if (chunk->metadata && chunk->metadata->count > 0) {
        size_t metadata_len = 0;
        if (nb_metadata_encode(chunk->metadata, task->metadata, sizeof(task->metadata), &metadata_len) !=
            NB_OK) {
            free(task);
            return NB_ERR_TOOBIG;
        }
        task->metadata_len = (uint16_t)metadata_len;
    }
    if (chunk->message && chunk->message[0]) {
        size_t message_len = strlen(chunk->message);
        if (message_len >= sizeof(task->message)) {
            message_len = sizeof(task->message) - 1;
        }
        memcpy(task->message, chunk->message, message_len);
        task->message[message_len] = '\0';
        task->message_len = (uint16_t)message_len;
    }
    if (chunk->len > 0) {
        task->data = (uint8_t *)malloc(chunk->len);
        if (!task->data) {
            free(task);
            return NB_ERR_NOMEM;
        }
        memcpy(task->data, chunk->data, chunk->len);
    }
    if (ntk_poller_async(stream->node->poller, nb_stream_send_async_fn, task, 1) != NTK_OK) {
        free(task->data);
        free(task);
        return NB_ERR_IO;
    }
    return NB_OK;
}

int nb_proxy_stream_cancel(nb_stream_t *stream)
{
    if (!stream || stream->closed) {
        return NB_ERR_NOTFOUND;
    }
    if (ntk_poller_is_current_thread(stream->node->poller)) {
        nb_stream_cancel_task_fn(stream);
        return NB_OK;
    }
    if (ntk_poller_async(stream->node->poller, nb_stream_cancel_task_fn, stream, 1) != NTK_OK) {
        return NB_ERR_IO;
    }
    return NB_OK;
}

int nb_proxy_stream_destroy(nb_stream_t *stream)
{
    if (!stream) {
        return NB_ERR_INVALID;
    }
    if (!stream->closed) {
        return NB_ERR_STATE;
    }
    free(stream);
    return NB_OK;
}
