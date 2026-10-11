/*
 * 服务端回复：把 status + payload 编码进响应帧并发出。
 *
 * 从 service.c 拆出，只负责“如何回复”，入口是 zrpc_reply / zrpc_reply_bytes /
 * zrpc_reply_stream 与错误状态回包 zrpc_service_send_status。
 */
#include "zrpc_service_internal.h"

#include "zrpc_envelope.h"
#include "zrpc_mem.h"
#include "zrpc_util.h"

#include <stdlib.h>
#include <string.h>

static int send_reply(zrpc_transport_t *transport, zrpc_peer_token_t token, const char *ip,
                      uint16_t port, uint32_t msg_id, const zrpc_payload_t *payload) {
    if (!transport) {
        return ZRPC_ERR_STATE;
    }
    return zrpc_transport_reply(transport, token, ip, port, ZRPC_KIND_RESPONSE, msg_id, NULL,
                                payload, NULL);
}

static int send_reply_stream(zrpc_transport_t *transport, zrpc_peer_token_t token, const char *ip,
                             uint16_t port, uint32_t msg_id, const zrpc_payload_t *payload,
                             const zrpc_stream_meta_t *stream) {
    if (!transport) {
        return ZRPC_ERR_STATE;
    }
    return zrpc_transport_reply(transport, token, ip, port, ZRPC_KIND_RESPONSE, msg_id, NULL,
                                payload, stream);
}

void zrpc_service_send_status(zrpc_transport_t *transport, zrpc_peer_token_t token, const char *ip,
                              uint16_t port, uint32_t msg_id, int status) {
    uint8_t buf[4];
    zrpc_payload_t pl;
    zrpc_put_u32(buf, (uint32_t)status);
    pl.data = buf;
    pl.len = sizeof(buf);
    pl.encoding = 0;
    (void)send_reply(transport, token, ip, port, msg_id, &pl);
}

/* 回复只在当前 poller 内同步发送，优先复用本地池中的临时 buffer。 */
static uint8_t *reply_buf_alloc(size_t need, size_t *cap_out, ztk_buf_pool **pool_out) {
    ztk_poller *poller = ztk_poller_current();
    ztk_buf_pool *pool = poller ? ztk_poller_buf_pool(poller) : NULL;
    void *buf;

    if (pool) {
        buf = ztk_buf_pool_acquire(pool, need, cap_out);
        if (buf) {
            *pool_out = pool;
            return (uint8_t *)buf;
        }
    }
    *pool_out = NULL;
    *cap_out = need;
    return (uint8_t *)malloc(need);
}

static void reply_buf_release(uint8_t *buf, size_t cap, ztk_buf_pool *pool) {
    if (!buf) {
        return;
    }
    if (pool) {
        ztk_buf_pool_release(pool, buf, cap);
    } else {
        free(buf);
    }
}

void zrpc_reply(zrpc_call_t *call, int status, const zrpc_payload_t *payload) {
    zrpc_node_t *node;
    uint8_t *buf;
    size_t buf_cap = 0;
    ztk_buf_pool *buf_pool = NULL;
    size_t len = (payload && payload->len) ? payload->len : 0;
    zrpc_payload_t out;

    if (!call || call->replied) {
        return;
    }
    call->replied = 1;
    node = call->node;
    if (!node || !call->transport) {
        zrpc_mem_free(call);
        return;
    }
    buf = reply_buf_alloc(4 + len, &buf_cap, &buf_pool);
    if (!buf) {
        zrpc_mem_free(call);
        return;
    }
    zrpc_put_u32(buf, (uint32_t)status);
    if (len > 0 && payload->data) {
        memcpy(buf + 4, payload->data, len);
    }
    out.data = buf;
    out.len = 4 + len;
    out.encoding = payload ? payload->encoding : 0;
    (void)send_reply(call->transport, call->token, call->peer_ip, call->peer_port, call->msg_id,
                     &out);
    reply_buf_release(buf, buf_cap, buf_pool);
    if (!call->stream_owned) {
        zrpc_mem_free(call);
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
    uint8_t *buf;
    size_t buf_cap = 0;
    ztk_buf_pool *buf_pool = NULL;
    zrpc_payload_t payload;
    zrpc_stream_meta_t stream;
    size_t len = chunk ? chunk->len : 0;
    int rc;
    if (!call || !call->node || !call->transport || (len > 0 && !chunk->data)) {
        return ZRPC_ERR_INVALID;
    }
    buf = reply_buf_alloc(4 + len, &buf_cap, &buf_pool);
    if (!buf) {
        return ZRPC_ERR_NOMEM;
    }
    zrpc_put_u32(buf, (uint32_t)status);
    if (len > 0) {
        memcpy(buf + 4, chunk->data, len);
    }
    payload.data = buf;
    payload.len = 4 + len;
    payload.encoding = chunk ? chunk->encoding : 0;
    memset(&stream, 0, sizeof(stream));
    stream.flags = chunk ? chunk->flags : ZRPC_STREAM_LAST;
    stream.stream_id = call->msg_id;
    stream.offset = chunk ? chunk->offset : 0;
    stream.total_size = 0;
    rc = send_reply_stream(call->transport, call->token, call->peer_ip, call->peer_port,
                           call->msg_id, &payload, &stream);
    reply_buf_release(buf, buf_cap, buf_pool);
    if (chunk && (chunk->flags & ZRPC_STREAM_LAST)) {
        call->replied = 1;
        if (!call->stream_owned) {
            zrpc_mem_free(call);
        }
    }
    return rc;
}
