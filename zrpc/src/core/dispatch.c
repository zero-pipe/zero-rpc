/*
 * 消息分发与出站传输选择。
 *
 * 从 node.c 拆出：入站把传输上抛的完整消息分派到 service（请求/事件）或
 * pending（响应）；出站按 scheme/transport 选中一条传输发送。
 */
#include "node_internal.h"

#include "envelope.h"
#include "pending.h"
#include "service_internal.h"
#include "util.h"

#include <string.h>

int zri_node_resolve(zrpc_node_t *node, const char *service, int prefer_stream,
                      zrpc_stub_target_t *out) {
    zrpc_transport_kind_t kind;
    zrpc_endpoint_t endpoint;

    if (!node || !service || !out) {
        return ZRPC_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    if (zri_route_lookup_prefer(&node->routes, service, prefer_stream, out->ip, sizeof(out->ip),
                                 &out->port, &kind, out->scheme, sizeof(out->scheme)) == ZRPC_OK) {
        out->transport = kind;
        return ZRPC_OK;
    }
    if (zri_registry_select_prefer(&node->registry, service, prefer_stream, &endpoint) !=
        ZRPC_OK) {
        return ZRPC_ERR_NOTFOUND;
    }
    zri_copy_str(out->ip, sizeof(out->ip), endpoint.host, "0.0.0.0");
    zri_copy_str(out->scheme, sizeof(out->scheme), endpoint.scheme, "udp");
    out->port = endpoint.port;
    out->transport = endpoint.transport;
    return ZRPC_OK;
}

static void node_dispatch(zrpc_node_t *node, const char *ip, uint16_t port, zri_peer_token_t token,
                          zri_transport_t *transport, uint8_t kind, int status, uint32_t msg_id,
                          const char *route, const zrpc_payload_t *payload,
                          const zrpc_stream_meta_t *stream) {
    if (kind == ZRPC_KIND_REQUEST || kind == ZRPC_KIND_EVENT) {
        if (stream) {
            zri_service_handle_stream_chunk(node, token, ip, transport, port, msg_id, route,
                                             stream, payload);
        } else {
            zri_service_handle_request(node, token, ip, transport, port, msg_id, route, payload);
        }
        return;
    }
    if (kind == ZRPC_KIND_RESPONSE) {
        zri_pending_t *p = zri_pending_find(node, msg_id);
        zrpc_response_t resp;
        if (!p) {
            return;
        }
        node->metrics.responses_received++;
        node->metrics.bytes_received += payload ? payload->len : 0;
        if (p->streaming) {
            int app_status = status;
            zrpc_chunk_t chunk;
            chunk.data = payload ? payload->data : NULL;
            chunk.len = payload ? payload->len : 0;
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
                zri_pending_release(p);
            }
            return;
        }
        resp.status = status;
        resp.payload.data = payload ? payload->data : NULL;
        resp.payload.len = payload ? payload->len : 0;
        resp.payload.encoding = payload ? payload->encoding : 0;
        zri_pending_invoke(p, ZRPC_OK, &resp);
        return;
    }
    /* EVENT: 预留，忽略 */
}

void zri_node_on_message(zrpc_node_t *node, const char *ip, uint16_t port, zri_peer_token_t token,
                          zri_transport_t *transport, uint8_t kind, int status, uint32_t msg_id,
                          const char *route, const zrpc_payload_t *payload,
                          const zrpc_stream_meta_t *stream) {
    if (!node) {
        return;
    }
    node_dispatch(node, ip, port, token, transport, kind, status, msg_id, route, payload, stream);
}

int zri_node_send(zrpc_node_t *node, const char *ip, uint16_t port, uint8_t kind, int status,
                   uint32_t msg_id, zrpc_transport_kind_t transport_kind, const char *scheme,
                   const char *route, const zrpc_payload_t *payload,
                   const zrpc_stream_meta_t *stream) {
    size_t i;
    if (!node || !ip || !port) {
        return ZRPC_ERR_INVALID;
    }
    for (i = 0; i < node->transport_count; i++) {
        zri_transport_t *transport = node->transports[i];
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
            return zri_transport_send(transport, ip, port, kind, status, msg_id, route, payload,
                                       stream);
        }
    }
    return ZRPC_ERR_NOTFOUND;
}
