/*
 * 服务端回复：把 status + payload 编码进响应帧并发出。
 *
 * 从 service.c 拆出，只负责“如何回复”，入口是 zrpc_reply / zrpc_reply_bytes /
 * zrpc_reply_stream 与错误状态回包 zri_service_send_status。
 */
#include "service_internal.h"

#include "envelope.h"
#include "mem.h"
#include "util.h"

#include <string.h>

static int send_reply(zri_transport_t *transport, zri_peer_token_t token, const char *ip,
                      uint16_t port, int status, uint32_t msg_id, const zrpc_payload_t *payload) {
    if (!transport) {
        return ZRPC_ERR_STATE;
    }
    return zri_transport_reply(transport, token, ip, port, ZRPC_KIND_RESPONSE, status, msg_id, NULL,
                                payload, NULL);
}

static int send_reply_stream(zri_transport_t *transport, zri_peer_token_t token, const char *ip,
                             uint16_t port, int status, uint32_t msg_id,
                             const zrpc_payload_t *payload, const zrpc_stream_meta_t *stream) {
    if (!transport) {
        return ZRPC_ERR_STATE;
    }
    return zri_transport_reply(transport, token, ip, port, ZRPC_KIND_RESPONSE, status, msg_id, NULL,
                                payload, stream);
}

void zri_service_send_status(zri_transport_t *transport, zri_peer_token_t token, const char *ip,
                              uint16_t port, uint32_t msg_id, int status) {
    zrpc_payload_t pl;
    memset(&pl, 0, sizeof(pl));
    (void)send_reply(transport, token, ip, port, status, msg_id, &pl);
}

void zrpc_reply(zrpc_call_t *call, int status, const zrpc_payload_t *payload) {
    if (!call || call->replied) {
        return;
    }
    call->replied = 1;
    if (!call->node || !call->transport) {
        zri_mem_free(call);
        return;
    }
    (void)send_reply(call->transport, call->token, call->peer_ip, call->peer_port, status,
                     call->msg_id, payload);
    if (!call->stream_owned) {
        zri_mem_free(call);
    }
}

void zrpc_reply_bytes(zrpc_call_t *call, int status, const void *data, size_t len) {
    zrpc_payload_t pl;
    pl.data = data;
    pl.len = len;
    pl.encoding = 0;
    zrpc_reply(call, status, &pl);
}

int zrpc_reply_stream(zrpc_call_t *call, int status, const zrpc_chunk_t *chunk) {
    zrpc_payload_t payload;
    zrpc_stream_meta_t stream;
    int rc;
    if (!call || !call->node || !call->transport || (chunk && chunk->len > 0 && !chunk->data)) {
        return ZRPC_ERR_INVALID;
    }
    memset(&payload, 0, sizeof(payload));
    if (chunk) {
        payload.data = chunk->data;
        payload.len = chunk->len;
        payload.encoding = chunk->encoding;
    }
    memset(&stream, 0, sizeof(stream));
    stream.flags = chunk ? chunk->flags : ZRPC_STREAM_LAST;
    stream.stream_id = call->msg_id;
    stream.offset = chunk ? chunk->offset : 0;
    stream.total_size = 0;
    rc = send_reply_stream(call->transport, call->token, call->peer_ip, call->peer_port, status,
                           call->msg_id, &payload, &stream);
    if (chunk && (chunk->flags & ZRPC_STREAM_LAST)) {
        call->replied = 1;
        if (!call->stream_owned) {
            zri_mem_free(call);
        }
    }
    return rc;
}
