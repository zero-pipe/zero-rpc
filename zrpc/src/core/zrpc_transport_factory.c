/* 传输装配：创建并启动一个 binding 的传输，登记进 node。 */
#include "zrpc_transport_factory.h"

#include "zrpc_node_internal.h"
#include "zrpc_transport.h"
#include "zrpc_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static zrpc_transport_t *create_transport(zrpc_transport_kind_t kind) {
    return kind == ZRPC_TRANSPORT_TCP ? zrpc_tcp_transport_create() : zrpc_udp_transport_create();
}

/* 自定义 provider 收到消息后，适配回 node 的统一分发入口。 */
static void provider_on_receive(void *receiver, void *reply_token, const char *peer_host,
                                uint16_t peer_port, const zrpc_transport_message_t *message) {
    zrpc_transport_t *transport = (zrpc_transport_t *)receiver;
    if (!transport || !transport->node || !message) {
        return;
    }
    zrpc_node_on_message(transport->node, peer_host, peer_port, reply_token, transport,
                         message->kind, message->request_id, message->route, &message->payload,
                         (message->stream.flags & ZRPC_STREAM_FIRST) ||
                                 (message->stream.flags & ZRPC_STREAM_LAST) ||
                                 (message->stream.flags & ZRPC_STREAM_CANCEL) ||
                                 message->stream.stream_id
                             ? &message->stream
                             : NULL);
}

int zrpc_node_transport_add(zrpc_node_t *node, const char *scheme, const char *host,
                            uint16_t port) {
    zrpc_transport_t *transport;
    zrpc_transport_kind_t kind;
    const zrpc_transport_provider_t *provider;
    char endpoint[ZRPC_ENDPOINT_MAX + ZRPC_SCHEME_MAX + 16];
    if (!node || !scheme || !host || !host[0] || node->transport_count >= ZRPC_MAX_BINDINGS) {
        return ZRPC_ERR_INVALID;
    }
    kind = zrpc_transport_kind_from_scheme(scheme);
    provider = zrpc_transport_provider_find(scheme);
    if (kind != ZRPC_TRANSPORT_TCP && kind != ZRPC_TRANSPORT_UDP && !provider) {
        return ZRPC_ERR_NOTFOUND; /* 未注册的 scheme */
    }
    transport = provider ? (zrpc_transport_t *)calloc(1, sizeof(*transport)) : create_transport(kind);
    if (!transport) {
        return ZRPC_ERR_NOMEM;
    }
    transport->node = node;
    zrpc_copy_str(transport->scheme, sizeof(transport->scheme), scheme, "udp");
    zrpc_copy_str(transport->bind_host, sizeof(transport->bind_host), host, "0.0.0.0");
    transport->port = port;
    /* 传输层只读的运行期配置快照（见 zrpc_transport.h）。 */
    transport->frag_bytes = node->frag_bytes;
    transport->max_msg_bytes = node->max_msg_bytes;
    transport->bp_high_water = node->bp_high_water;
    transport->drop_percent = node->drop_percent;
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
    if (zrpc_transport_start(transport) != ZRPC_OK) {
        zrpc_transport_destroy(transport);
        return ZRPC_ERR_IO;
    }
    node->transports[node->transport_count++] = transport;
    if (!node->transport) {
        node->transport = transport;
    }
    return ZRPC_OK;
}
