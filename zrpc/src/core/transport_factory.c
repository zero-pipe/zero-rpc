/* 传输装配：创建并启动一个 binding 的传输，登记进 node。 */
#include "transport_factory.h"

#include "node_internal.h"
#include "transport.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static zri_transport_t *create_transport(zrpc_transport_kind_t kind) {
    return kind == ZRPC_TRANSPORT_TCP ? zri_tcp_transport_create() : zri_udp_transport_create();
}

/* 自定义 provider 收到消息后，适配回 node 的统一分发入口。 */
static void provider_on_receive(void *receiver, void *reply_token, const char *peer_host,
                                uint16_t peer_port, const zrpc_transport_message_t *message) {
    zri_transport_t *transport = (zri_transport_t *)receiver;
    if (!transport || !transport->node || !message) {
        return;
    }
    zri_node_on_message(transport->node, peer_host, peer_port, reply_token, transport,
                         message->kind, message->status, message->request_id, message->route,
                         &message->payload,
                         (message->stream.flags & ZRPC_STREAM_FIRST) ||
                                 (message->stream.flags & ZRPC_STREAM_LAST) ||
                                 (message->stream.flags & ZRPC_STREAM_CANCEL) ||
                                 message->stream.stream_id
                             ? &message->stream
                             : NULL);
}

/*
 * provider 适配层：把外部 provider SPI 适配成统一的 zri_transport_ops_t，
 * 使 node/upper 层只面对单一传输接口（不再有 provider/ops 双分支）。
 */
static int provider_op_start(zri_transport_t *t) {
    return t->provider->ops->start(t->provider_instance);
}

static void provider_op_stop(zri_transport_t *t) {
    t->provider->ops->stop(t->provider_instance);
}

static int provider_op_send(zri_transport_t *t, const char *ip, uint16_t port, uint8_t kind,
                            int status, uint32_t msg_id, const char *route,
                            const zrpc_payload_t *payload, const zrpc_stream_meta_t *stream) {
    zrpc_transport_message_t message;
    message.kind = kind;
    message.status = status;
    message.request_id = msg_id;
    message.stream = stream ? *stream : (zrpc_stream_meta_t){0};
    message.route = route;
    message.payload = payload ? *payload : (zrpc_payload_t){0};
    return t->provider->ops->send(t->provider_instance, ip, port, &message);
}

static int provider_op_reply(zri_transport_t *t, zri_peer_token_t token, const char *ip,
                             uint16_t port, uint8_t kind, int status, uint32_t msg_id,
                             const char *route, const zrpc_payload_t *payload,
                             const zrpc_stream_meta_t *stream) {
    zrpc_transport_message_t message;
    message.kind = kind;
    message.status = status;
    message.request_id = msg_id;
    message.stream = stream ? *stream : (zrpc_stream_meta_t){0};
    message.route = route;
    message.payload = payload ? *payload : (zrpc_payload_t){0};
    return t->provider->ops->reply(t->provider_instance, token, ip, port, &message);
}

static int provider_op_rtcp_peer(zri_transport_t *t, const char *ip, uint16_t port,
                                 zrpc_rtcp_peer_stats_t *out) {
    (void)t;
    (void)ip;
    (void)port;
    (void)out;
    return 0;
}

static const zri_transport_ops_t g_provider_ops = {
    provider_op_start,
    provider_op_stop,
    provider_op_send,
    provider_op_reply,
    provider_op_rtcp_peer,
};

int zri_node_transport_add(zrpc_node_t *node, const char *scheme, const char *host,
                            uint16_t port) {
    zri_transport_t *transport;
    zrpc_transport_kind_t kind;
    const zrpc_transport_provider_t *provider;
    char endpoint[ZRPC_ENDPOINT_MAX + ZRPC_SCHEME_MAX + 16];
    if (!node || !scheme || !host || !host[0] || node->transport_count >= ZRPC_MAX_BINDINGS) {
        return ZRPC_ERR_INVALID;
    }
    kind = zrpc_transport_kind_from_scheme(scheme);
    provider = zri_transport_provider_find(scheme);
    if (kind != ZRPC_TRANSPORT_TCP && kind != ZRPC_TRANSPORT_UDP && !provider) {
        return ZRPC_ERR_NOTFOUND; /* 未注册的 scheme */
    }
    transport = provider ? (zri_transport_t *)calloc(1, sizeof(*transport)) : create_transport(kind);
    if (!transport) {
        return ZRPC_ERR_NOMEM;
    }
    transport->node = node;
    zri_copy_str(transport->scheme, sizeof(transport->scheme), scheme, "udp");
    zri_copy_str(transport->bind_host, sizeof(transport->bind_host), host, "0.0.0.0");
    transport->port = port;
    /* 传输层只读的运行期配置快照（见 zri_transport.h）。 */
    transport->frag_bytes = node->frag_bytes;
    transport->max_msg_bytes = node->max_msg_bytes;
    transport->bp_high_water = node->bp_high_water;
    transport->drop_percent = node->drop_percent;
    transport->rtcp_interval_ms = node->rtcp_interval_ms;
    transport->rtcp_disabled = node->rtcp_disabled;
    transport->io_threads = node->io_threads;
    transport->pool = node->pool;
    transport->poller = node->poller;
    transport->metrics = &node->metrics;
    if (provider) {
        zrpc_transport_provider_options_t options;
        zrpc_transport_instance_t *instance = NULL;
        int n = snprintf(endpoint, sizeof(endpoint), "%s://%s:%u", scheme, host, (unsigned)port);
        if (n < 0 || (size_t)n >= sizeof(endpoint)) {
            free(transport);
            return ZRPC_ERR_TOOBIG;
        }
        transport->kind = ZRPC_TRANSPORT_CUSTOM;
        transport->ops = &g_provider_ops;
        transport->provider = provider;
        memset(&options, 0, sizeof(options));
        options.scheme = scheme;
        options.endpoint = endpoint;
        options.receiver = transport;
        options.on_receive = provider_on_receive;
        if (provider->ops->create(&options, &instance) != ZRPC_OK || !instance) {
            free(transport);
            return ZRPC_ERR_IO;
        }
        transport->provider_instance = instance;
    }
    if (zri_transport_start(transport) != ZRPC_OK) {
        zri_transport_destroy(transport);
        return ZRPC_ERR_IO;
    }
    node->transports[node->transport_count++] = transport;
    if (!node->transport) {
        node->transport = transport;
    }
    return ZRPC_OK;
}
