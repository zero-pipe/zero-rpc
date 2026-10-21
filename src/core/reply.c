/*
 * 服务端回复：把 status + payload 编码进响应帧并发出。
 *
 * 从 service.c 拆出，只负责“如何回复”，入口是 reply / reply_bytes /
 * reply_stream 与错误状态回包 zri_service_send_status。
 */
#include "service_internal.h"
#include "node_internal.h"

#include "envelope.h"
#include "mem.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

/*
 * 由请求上下文派生响应上下文：保留路由/回程 token，切换 kind 与 payload。
 * 这样 reply 路径与普通发送共用同一个 zri_message_t 抽象。
 */
static int send_reply(zri_transport_t *transport,
                      const zri_message_t *request,
                      int status,
                      const void *metadata,
                      uint16_t metadata_len,
                      const char *message,
                      uint16_t message_len,
                      const payload_t *payload,
                      const stream_meta_t *stream)
{
    zri_message_t reply;
    if (!transport || !request) {
        return ERR_STATE;
    }
    /* 传输层状态消息缓冲固定为 STATUS_MSG_MAX，这里统一截断避免越界。 */
    if (message_len > STATUS_MSG_MAX) {
        message_len = STATUS_MSG_MAX;
    }
    reply = *request;
    reply.kind = KIND_RESPONSE;
    reply.status = status;
    reply.route = NULL;
    reply.metadata = metadata;
    reply.metadata_len = metadata_len;
    reply.status_msg = message;
    reply.status_msg_len = message_len;
    reply.payload = payload ? *payload : (payload_t){ 0 };
    if (stream) {
        reply.stream = *stream;
        reply.has_stream = 1;
    }
    return zri_transport_reply(transport, &reply);
}

/* 从 call_t 还原请求上下文（异步 reply 时请求视图已失效，只保留寻址信息）。 */
static void call_request_context(const call_t *call, zri_message_t *out)
{
    memset(out, 0, sizeof(*out));
    out->request_id = call->msg_id;
    out->peer_ip = call->peer_ip;
    out->peer_port = call->peer_port;
    out->peer_token = call->token;
}

/*
 * call 可能来自 poller 的 zri_mem 池，也可能来自 worker 复用池；统一交给
 * zri_worker_call_free 按来源回收，避免对池对象误用 zri_mem_free。
 */
static void call_release(call_t *call)
{
    if (call) {
        zri_worker_call_free(call->node, call);
    }
}

/*
 * 跨线程回复任务。handler 在 worker 线程执行时，reply() 不能直接碰传输
 * （UDP/TCP 的 session/link 都是 per-poller 无锁状态），必须把发送回投到
 * 请求到达的 poller。任务自带寻址上下文，不依赖 call 的生命周期。
 */
typedef struct zri_reply_task {
    node_t *node;
    zri_transport_t *transport;
    zri_peer_token_t token;
    char peer_ip[ENDPOINT_MAX];
    uint16_t peer_port;
    uint32_t msg_id;
    int status;
    uint8_t metadata[METADATA_WIRE_MAX];
    uint16_t metadata_len;
    char message[STATUS_MSG_MAX];
    uint16_t message_len;
    uint8_t *data;
    uint8_t inline_data[128]; /* 小响应免一次 malloc */
    size_t len;
    uint8_t encoding;
    int has_stream;
    unsigned stream_flags;
    uint32_t stream_id;
    uint64_t stream_offset;
} zri_reply_task_t;

static void reply_task_destroy(zri_reply_task_t *t)
{
    if (!t) {
        return;
    }
    if (t->data && t->data != t->inline_data) {
        free(t->data);
    }
    if (t->node && t->node->reply_task_pool.lock) {
        zri_obj_pool_free(&t->node->reply_task_pool, t);
    } else {
        free(t);
    }
}

static void reply_task_fn(void *user)
{
    zri_reply_task_t *t = (zri_reply_task_t *)user;
    zri_message_t request;
    payload_t payload;
    stream_meta_t stream;
    const stream_meta_t *stream_ptr = NULL;

    memset(&request, 0, sizeof(request));
    request.request_id = t->msg_id;
    request.peer_ip = t->peer_ip;
    request.peer_port = t->peer_port;
    request.peer_token = t->token;
    request.metadata = t->metadata_len ? t->metadata : NULL;
    request.metadata_len = t->metadata_len;
    request.status_msg = t->message_len ? t->message : NULL;
    request.status_msg_len = t->message_len;

    memset(&payload, 0, sizeof(payload));
    payload.data = t->data;
    payload.len = t->len;
    payload.encoding = t->encoding;

    if (t->has_stream) {
        memset(&stream, 0, sizeof(stream));
        stream.flags = t->stream_flags;
        stream.stream_id = t->stream_id;
        stream.offset = t->stream_offset;
        stream_ptr = &stream;
    }
    (void)send_reply(t->transport,
                     &request,
                     t->status,
                     t->metadata_len ? t->metadata : NULL,
                     t->metadata_len,
                     t->message_len ? t->message : NULL,
                     t->message_len,
                     &payload,
                     stream_ptr);
    reply_task_destroy(t);
}

/*
 * 把回复拷成任务回投 call->poller。release_call 非 0 时，任务接管 call
 * （unary 回复 / STREAM_LAST）；分块响应的中间 chunk 保留 call。
 */
static int reply_marshal(call_t *call,
                         int status,
                         const payload_t *payload,
                         const stream_meta_t *stream,
                         int release_call,
                         const metadata_t *metadata,
                         const char *message)
{
    zri_reply_task_t *t;
    int free_call = release_call && !call->stream_owned;

    if (!call->poller) {
        if (free_call) {
            call_release(call);
        }
        return ERR_STATE;
    }
    t = call->node && call->node->reply_task_pool.lock ?
        (zri_reply_task_t *)zri_obj_pool_alloc(&call->node->reply_task_pool, sizeof(*t)) :
        (zri_reply_task_t *)calloc(1, sizeof(*t));
    if (!t) {
        if (free_call) {
            call_release(call);
        }
        return ERR_NOMEM;
    }
    memset(t, 0, sizeof(*t));
    t->node = call->node;
    t->transport = call->transport;
    t->token = call->token;
    zri_copy_str(t->peer_ip, sizeof(t->peer_ip), call->peer_ip, "0.0.0.0");
    t->peer_port = call->peer_port;
    t->msg_id = call->msg_id;
    t->status = status;
    if (metadata && metadata->count > 0) {
        size_t meta_len = 0;
        if (metadata_encode(metadata, t->metadata, sizeof(t->metadata), &meta_len) == OK) {
            t->metadata_len = (uint16_t)meta_len;
        }
    }
    if (message && message[0]) {
        size_t msg_len = strlen(message);
        if (msg_len >= sizeof(t->message)) {
            msg_len = sizeof(t->message) - 1;
        }
        memcpy(t->message, message, msg_len);
        t->message[msg_len] = '\0';
        t->message_len = (uint16_t)msg_len;
    }
    if (payload && payload->len > 0 && payload->data) {
        t->data = payload->len <= sizeof(t->inline_data) ? t->inline_data :
                                                           (uint8_t *)malloc(payload->len);
        if (!t->data) {
            reply_task_destroy(t);
            if (free_call) {
                call_release(call);
            }
            return ERR_NOMEM;
        }
        memcpy(t->data, payload->data, payload->len);
        t->len = payload->len;
        t->encoding = payload->encoding;
    }
    if (stream) {
        t->has_stream = 1;
        t->stream_flags = stream->flags;
        t->stream_id = stream->stream_id;
        t->stream_offset = stream->offset;
    }
    if (ntk_poller_async(call->poller, reply_task_fn, t, 1) != NTK_OK) {
        reply_task_destroy(t);
        if (free_call) {
            call_release(call);
        }
        return ERR_IO;
    }
    if (free_call) {
        call_release(call);
    }
    return OK;
}

void zri_service_send_status(zri_transport_t *transport, const zri_message_t *request, int status)
{
    payload_t empty;
    if (!request || request->kind == KIND_EVENT) {
        return;
    }
    memset(&empty, 0, sizeof(empty));
    (void)send_reply(transport, request, status, NULL, 0, NULL, 0, &empty, NULL);
}

/* ---- 回复 ---- */

void reply_ex(call_t *call,
              int status,
              const char *message,
              const payload_t *payload,
              const metadata_t *metadata)
{
    zri_message_t request;
    uint8_t meta_buf[METADATA_WIRE_MAX];
    size_t meta_len = 0;
    if (!call || call->replied) {
        return;
    }
    call->replied = 1;
    if (call->one_way) {
        return;
    }
    if (!call->node || !call->transport) {
        call_release(call);
        return;
    }
    if (metadata && metadata->count > 0) {
        if (metadata_encode(metadata, meta_buf, sizeof(meta_buf), &meta_len) != OK) {
            meta_len = 0;
        }
    }
    if (call->poller && !ntk_poller_is_current_thread(call->poller)) {
        (void)reply_marshal(call, status, payload, NULL, 1, metadata, message);
        return;
    }
    call_request_context(call, &request);
    (void)send_reply(call->transport,
                     &request,
                     status,
                     meta_len ? meta_buf : NULL,
                     (uint16_t)meta_len,
                     message,
                     message ? (uint16_t)strlen(message) : 0,
                     payload,
                     NULL);
    if (!call->stream_owned) {
        call_release(call);
    }
}

void reply(call_t *call, int status, const payload_t *payload)
{
    reply_ex(call, status, NULL, payload, NULL);
}

void reply_bytes(call_t *call, int status, const void *data, size_t len)
{
    payload_t pl;
    pl.data = data;
    pl.len = len;
    pl.encoding = 0;
    reply(call, status, &pl);
}

int reply_stream(call_t *call, int status, const chunk_t *chunk)
{
    zri_message_t request;
    payload_t payload;
    stream_meta_t stream;
    uint8_t meta_buf[METADATA_WIRE_MAX];
    size_t meta_len = 0;
    int rc;
    if (!call || !call->node || !call->transport || (chunk && chunk->len > 0 && !chunk->data)) {
        return ERR_INVALID;
    }
    memset(&payload, 0, sizeof(payload));
    if (chunk) {
        payload.data = chunk->data;
        payload.len = chunk->len;
        payload.encoding = chunk->encoding;
    }
    memset(&stream, 0, sizeof(stream));
    stream.flags = chunk ? chunk->flags : STREAM_LAST;
    stream.stream_id = call->msg_id;
    stream.offset = chunk ? chunk->offset : 0;
    stream.total_size = 0;

    if (call->poller && !ntk_poller_is_current_thread(call->poller)) {
        int last = chunk && (chunk->flags & STREAM_LAST);
        if (last) {
            call->replied = 1;
        }
        return reply_marshal(call,
                             status,
                             &payload,
                             &stream,
                             last,
                             chunk ? chunk->metadata : NULL,
                             chunk ? chunk->message : NULL);
    }

    if (chunk && chunk->metadata && chunk->metadata->count > 0) {
        if (metadata_encode(chunk->metadata, meta_buf, sizeof(meta_buf), &meta_len) != OK) {
            meta_len = 0;
        }
    }
    call_request_context(call, &request);
    rc = send_reply(call->transport,
                    &request,
                    status,
                    meta_len ? meta_buf : NULL,
                    (uint16_t)meta_len,
                    chunk ? chunk->message : NULL,
                    chunk && chunk->message ? (uint16_t)strlen(chunk->message) : 0,
                    &payload,
                    &stream);
    if (chunk && (chunk->flags & STREAM_LAST)) {
        call->replied = 1;
        if (!call->stream_owned) {
            call_release(call);
        }
    }
    return rc;
}
