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

static void node_dispatch(zrpc_node_t *node, zri_transport_t *transport,
                          const zri_message_t *msg) {
    if (msg->kind == ZRPC_KIND_REQUEST || msg->kind == ZRPC_KIND_EVENT) {
        if (msg->has_stream) {
            zri_service_handle_stream_chunk(node, transport, msg);
        } else {
            zri_service_handle_request(node, transport, msg);
        }
        return;
    }
    if (msg->kind == ZRPC_KIND_RESPONSE) {
        zri_pending_t *p = zri_pending_find(node, msg->request_id);
        zrpc_response_t resp;
        if (!p) {
            return;
        }
        node->metrics.responses_received++;
        node->metrics.bytes_received += msg->payload.len;
        if (p->streaming) {
            int app_status = msg->status;
            zrpc_chunk_t chunk;
            chunk.data = msg->payload.data;
            chunk.len = msg->payload.len;
            chunk.encoding = msg->payload.encoding;
            chunk.offset = msg->has_stream ? msg->stream.offset : p->expected_offset;
            chunk.flags = msg->has_stream ? msg->stream.flags : 0;
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
        resp.status = msg->status;
        resp.payload = msg->payload;
        zri_pending_invoke(p, ZRPC_OK, &resp);
        return;
    }
    /* EVENT: 预留，忽略 */
}

void zri_node_on_message(zrpc_node_t *node, zri_transport_t *transport,
                         const zri_message_t *msg) {
    if (!node || !transport || !msg) {
        return;
    }
    node_dispatch(node, transport, msg);
}

int zri_node_send(zrpc_node_t *node, zrpc_transport_kind_t transport_kind, const char *scheme,
                   const zri_message_t *msg) {
    size_t i;
    if (!node || !msg || !msg->peer_ip || !msg->peer_port) {
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
        return zri_transport_send(transport, msg);
    }
    return ZRPC_ERR_NOTFOUND;
}

int zri_node_send_call(zrpc_node_t *node, const zrpc_stub_target_t *target, const char *service,
                       const char *method, const zrpc_payload_t *payload, uint8_t kind,
                       uint64_t request_id) {
    zri_message_t message;
    char route[ZRPC_ROUTE_MAX];

    if (!node || !target || !service || !method || !target->ip[0] || !target->port) {
        return ZRPC_ERR_INVALID;
    }
    if (zri_route_format(route, sizeof(route), service, method) != ZRPC_OK) {
        return ZRPC_ERR_INVALID;
    }
    memset(&message, 0, sizeof(message));
    message.kind = kind;
    message.request_id = (uint32_t)request_id;
    message.route = route;
    message.payload = payload ? *payload : (zrpc_payload_t){0};
    message.peer_ip = target->ip;
    message.peer_port = target->port;
    return zri_node_send(node, target->transport, target->scheme, &message);
}
