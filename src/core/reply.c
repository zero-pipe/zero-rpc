/*
 * 服务端回复：把 status + payload 编码进响应帧并发出。
 *
 * 从 service.c 拆出，只负责“如何回复”，入口是 reply / reply_bytes /
 * reply_stream 与错误状态回包 zri_service_send_status。
 */
#include "service_internal.h"

#include "envelope.h"
#include "mem.h"
#include "util.h"

#include <string.h>

/*
 * 由请求上下文派生响应上下文：保留路由/回程 token，切换 kind 与 payload。
 * 这样 reply 路径与普通发送共用同一个 zri_message_t 抽象。
 */
static int send_reply(zri_transport_t *transport, const zri_message_t *request, int status,
                      const payload_t *payload, const stream_meta_t *stream) {
    zri_message_t reply;
    if (!transport || !request) {
        return ERR_STATE;
    }
    reply = *request;
    reply.kind = KIND_RESPONSE;
    reply.status = status;
    reply.route = NULL;
    reply.payload = payload ? *payload : (payload_t){0};
    if (stream) {
        reply.stream = *stream;
        reply.has_stream = 1;
    }
    return zri_transport_reply(transport, &reply);
}

/* 从 call_t 还原请求上下文（异步 reply 时请求视图已失效，只保留寻址信息）。 */
static void call_request_context(const call_t *call, zri_message_t *out) {
    memset(out, 0, sizeof(*out));
    out->request_id = call->msg_id;
    out->peer_ip = call->peer_ip;
    out->peer_port = call->peer_port;
    out->peer_token = call->token;
}

void zri_service_send_status(zri_transport_t *transport, const zri_message_t *request, int status) {
    payload_t empty;
    if (!request || request->kind == KIND_EVENT) {
        return;
    }
    memset(&empty, 0, sizeof(empty));
    (void)send_reply(transport, request, status, &empty, NULL);
}

/* ---- 回复 ---- */

void reply(call_t *call, int status, const payload_t *payload) {
    zri_message_t request;
    if (!call || call->replied) {
        return;
    }
    call->replied = 1;
    if (call->one_way) {
        return;
    }
    if (!call->node || !call->transport) {
        zri_mem_free(call);
        return;
    }
    call_request_context(call, &request);
    (void)send_reply(call->transport, &request, status, payload, NULL);
    if (!call->stream_owned) {
        zri_mem_free(call);
    }
}

void reply_bytes(call_t *call, int status, const void *data, size_t len) {
    payload_t pl;
    pl.data = data;
    pl.len = len;
    pl.encoding = 0;
    reply(call, status, &pl);
}

int reply_stream(call_t *call, int status, const chunk_t *chunk) {
    zri_message_t request;
    payload_t payload;
    stream_meta_t stream;
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
    call_request_context(call, &request);
    rc = send_reply(call->transport, &request, status, &payload, &stream);
    if (chunk && (chunk->flags & STREAM_LAST)) {
        call->replied = 1;
        if (!call->stream_owned) {
            zri_mem_free(call);
        }
    }
    return rc;
}
