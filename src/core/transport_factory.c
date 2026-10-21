/* 传输装配：创建并启动一个 binding 的传输，登记进 node。 */
#include "transport_factory.h"

#include "node_internal.h"
#include "transport_internal.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static zri_transport_t *create_transport(transport_kind_t kind)
{
    return kind == TRANSPORT_TCP ? zri_tcp_transport_create() : zri_udp_transport_create();
}

/* 自定义 provider 收到消息后，适配回 node 的统一分发入口。 */
static void provider_on_receive(void *receiver,
                                void *reply_token,
                                const char *peer_host,
                                uint16_t peer_port,
                                const transport_message_t *message)
{
    zri_transport_t *transport = (zri_transport_t *)receiver;
    zri_message_t msg;
    int has_stream;
    if (!transport || !transport->node || !message) {
        return;
    }
    has_stream = (message->stream.flags & (STREAM_FIRST | STREAM_LAST | STREAM_CANCEL)) != 0 ||
        message->stream.stream_id != 0;
    memset(&msg, 0, sizeof(msg));
    msg.kind = message->kind;
    msg.status = message->status;
    msg.request_id = message->request_id;
    msg.route = message->route;
    msg.payload = message->payload;
    if (has_stream) {
        msg.stream = message->stream;
        msg.has_stream = 1;
    }
    msg.peer_ip = peer_host;
    msg.peer_port = peer_port;
    msg.peer_token = reply_token;
    zri_node_on_message(transport->node, transport, &msg);
}

/*
 * provider 适配层：把外部 provider SPI 适配成统一的 zri_transport_ops_t，
 * 使 node/upper 层只面对单一传输接口（不再有 provider/ops 双分支）。
 */
static int provider_op_start(zri_transport_t *t)
{
    return t->provider->ops->start(t->provider_instance);
}

static void provider_op_stop(zri_transport_t *t)
{
    t->provider->ops->stop(t->provider_instance);
}

/* 把统一消息上下文转换成公开 provider SPI 的消息结构。 */
static void provider_message_from(const zri_message_t *src, transport_message_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->kind = src->kind;
    dst->status = src->status;
    dst->request_id = src->request_id;
    if (src->has_stream) {
        dst->stream = src->stream;
    }
    dst->route = src->route;
    dst->payload = src->payload;
}

static int provider_op_send(zri_transport_t *t, const zri_message_t *message)
{
    transport_message_t provider_message;
    if (!t || !t->provider || !message) {
        return ERR_INVALID;
    }
    provider_message_from(message, &provider_message);
    return t->provider->ops->send(
        t->provider_instance, message->peer_ip, message->peer_port, &provider_message);
}

static int provider_op_reply(zri_transport_t *t, const zri_message_t *message)
{
    transport_message_t provider_message;
    if (!t || !t->provider || !message) {
        return ERR_INVALID;
    }
    provider_message_from(message, &provider_message);
    return t->provider->ops->reply(t->provider_instance,
                                   message->peer_token,
                                   message->peer_ip,
                                   message->peer_port,
                                   &provider_message);
}

static int provider_op_peer_stats(zri_transport_t *t,
                                  const char *ip,
                                  uint16_t port,
                                  peer_stats_t *out)
{
    (void)t;
    (void)ip;
    (void)port;
    (void)out;
    return 0;
}

static const zri_transport_ops_t g_provider_ops = {
    provider_op_start, provider_op_stop,       provider_op_send,
    provider_op_reply, provider_op_peer_stats,
};

int zri_node_transport_add(node_t *node, const char *scheme, const char *host, uint16_t port)
{
    zri_transport_t *transport;
    transport_kind_t kind;
    const transport_provider_t *provider;
    char endpoint[ENDPOINT_MAX + SCHEME_MAX + 16];
    if (!node || !scheme || !host || !host[0] || node->transport_count >= MAX_BINDINGS) {
        return ERR_INVALID;
    }
    kind = transport_kind_from_scheme(scheme);
    provider = zri_transport_provider_find(scheme);
    if (kind != TRANSPORT_TCP && kind != TRANSPORT_UDP && !provider) {
        return ERR_NOTFOUND; /* 未注册的 scheme */
    }
    transport =
        provider ? (zri_transport_t *)calloc(1, sizeof(*transport)) : create_transport(kind);
    if (!transport) {
        return ERR_NOMEM;
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
        transport_provider_options_t options;
        transport_instance_t *instance = NULL;
        int n = snprintf(endpoint, sizeof(endpoint), "%s://%s:%u", scheme, host, (unsigned)port);
        if (n < 0 || (size_t)n >= sizeof(endpoint)) {
            free(transport);
            return ERR_TOOBIG;
        }
        transport->kind = TRANSPORT_CUSTOM;
        transport->ops = &g_provider_ops;
        transport->provider = provider;
        memset(&options, 0, sizeof(options));
        options.scheme = scheme;
        options.endpoint = endpoint;
        options.receiver = transport;
        options.on_receive = provider_on_receive;
        if (provider->ops->create(&options, &instance) != OK || !instance) {
            free(transport);
            return ERR_IO;
        }
        transport->provider_instance = instance;
    }
    if (zri_transport_start(transport) != OK) {
        zri_transport_destroy(transport);
        return ERR_IO;
    }
    node->transports[node->transport_count++] = transport;
    return OK;
}
