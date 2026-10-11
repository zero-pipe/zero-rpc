/*
 * 消息分发与出站传输选择。
 *
 * 从 node.c 拆出：入站把传输上抛的完整消息分派到 service（请求/事件）或
 * pending（响应）；出站按 scheme/transport 选中一条传输发送。
 */
#include "zrpc_node_internal.h"

#include "zrpc_envelope.h"
#include "zrpc_pending.h"
#include "zrpc_service_internal.h"
#include "zrpc_util.h"

#include <string.h>

int zrpc_node_resolve(zrpc_node_t *node, const char *service, int prefer_stream,
                      zrpc_stub_target_t *out) {
    zrpc_transport_kind_t kind;
    zrpc_endpoint_t endpoint;

    if (!node || !service || !out) {
        return ZRPC_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    if (zrpc_route_lookup_prefer(&node->routes, service, prefer_stream, out->ip, sizeof(out->ip),
                                 &out->port, &kind, out->scheme, sizeof(out->scheme)) == ZRPC_OK) {
        out->transport = kind;
        return ZRPC_OK;
    }
    if (zrpc_registry_select_prefer(&node->registry, service, prefer_stream, &endpoint) !=
        ZRPC_OK) {
        return ZRPC_ERR_NOTFOUND;
    }
    zrpc_copy_str(out->ip, sizeof(out->ip), endpoint.host, "0.0.0.0");
    zrpc_copy_str(out->scheme, sizeof(out->scheme), endpoint.scheme, "udp");
    out->port = endpoint.port;
    out->transport = endpoint.transport;
    return ZRPC_OK;
}

static void node_dispatch(zrpc_node_t *node, const char *ip, uint16_t port, zrpc_peer_token_t token,
                          zrpc_transport_t *transport, uint8_t kind, uint32_t msg_id,
                          const char *route, const zrpc_payload_t *payload,
                          const zrpc_stream_meta_t *stream) {
    if (kind == ZRPC_KIND_REQUEST || kind == ZRPC_KIND_EVENT) {
        if (stream) {
            zrpc_service_handle_stream_chunk(node, token, ip, transport, port, msg_id, route,
                                             stream, payload);
        } else {
            zrpc_service_handle_request(node, token, ip, transport, port, msg_id, route, payload);
        }
        return;
    }
    if (kind == ZRPC_KIND_RESPONSE) {
        zrpc_pending_t *p = zrpc_pending_find(node, msg_id);
        zrpc_response_t resp;
        if (!p) {
            return;
        }
        node->metrics.responses_received++;
        node->metrics.bytes_received += payload ? payload->len : 0;
        if (p->streaming) {
            int app_status = 0;
            zrpc_chunk_t chunk;
            if (payload && payload->len >= 4 && payload->data) {
                app_status = (int)zrpc_get_u32((const uint8_t *)payload->data);
            }
            chunk.data = (payload && payload->len >= 4) ? (const uint8_t *)payload->data + 4 : NULL;
            chunk.len = (payload && payload->len >= 4) ? payload->len - 4 : 0;
            chunk.encoding = payload ? payload->encoding : 0;
            chunk.offset = stream ? stream->offset : p->expected_offset;
            chunk.flags = stream ? stream->flags : 0;
            if (p->chunk_cb) {
                p->chunk_cb(app_status, &chunk, p->stream_user);
            }
            p->expected_offset = chunk.offset + chunk.len;
            if (chunk.flags & ZRPC_STREAM_LAST) {
                zrpc_stream_done_fn done = p->done_cb;
                void *stream_user = p->stream_user;
                p->chunk_cb = NULL;
                p->done_cb = NULL;
                if (done) {
                    done(app_status, stream_user);
                }
                zrpc_pending_release(p);
            }
            return;
        }
        if (!payload || payload->len < 4 || !payload->data) {
            zrpc_pending_invoke(p, ZRPC_ERR_INVALID, NULL);
            return;
        }
        resp.status = (int)zrpc_get_u32((const uint8_t *)payload->data);
        resp.payload.data = (const uint8_t *)payload->data + 4;
        resp.payload.len = payload->len - 4;
        resp.payload.encoding = payload->encoding;
        zrpc_pending_invoke(p, ZRPC_OK, &resp);
        return;
    }
    /* EVENT: 预留，忽略 */
}

void zrpc_node_on_message(zrpc_node_t *node, const char *ip, uint16_t port, zrpc_peer_token_t token,
                          zrpc_transport_t *transport, uint8_t kind, uint32_t msg_id,
                          const char *route, const zrpc_payload_t *payload,
                          const zrpc_stream_meta_t *stream) {
    if (!node) {
        return;
    }
    node_dispatch(node, ip, port, token, transport, kind, msg_id, route, payload, stream);
}

int zrpc_node_send(zrpc_node_t *node, const char *ip, uint16_t port, uint8_t kind, uint32_t msg_id,
                   zrpc_transport_kind_t transport_kind, const char *scheme, const char *route,
                   const zrpc_payload_t *payload, const zrpc_stream_meta_t *stream) {
    size_t i;
    if (!node || !ip || !port) {
        return ZRPC_ERR_INVALID;
    }
    for (i = 0; i < node->transport_count; i++) {
        zrpc_transport_t *transport = node->transports[i];
        if (!transport) {
            continue;
        }
        if (scheme && scheme[0] && strcmp(transport->scheme, scheme) != 0) {
            continue;
        }
        if ((!scheme || !scheme[0]) && transport->kind != transport_kind) {
            continue;
        }
        if (transport->provider || transport->ops) {
            return zrpc_transport_send(transport, ip, port, kind, msg_id, route, payload, stream);
        }
    }
    return ZRPC_ERR_NOTFOUND;
}
