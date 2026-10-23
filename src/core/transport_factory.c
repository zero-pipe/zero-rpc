/* 传输装配：创建并启动一个 binding 的传输，登记进 node。 */
#include "transport_factory.h"

#include "node_internal.h"
#include "transport_internal.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nb_transport_t *nb_create_transport(nb_transport_kind_t kind)
{
    return kind == NB_TRANSPORT_TCP ? nb_tcp_transport_create() : nb_udp_transport_create();
}

/* 自定义 provider 收到消息后，适配回 node 的统一分发入口。 */
static void nb_provider_on_receive(void *receiver,
                                void *reply_token,
                                const char *peer_host,
                                uint16_t peer_port,
                                const nb_transport_message_t *message)
{
    nb_transport_t *transport = (nb_transport_t *)receiver;
    nb_message_t msg;
    int has_stream;
    if (!transport || !transport->node || !message) {
        return;
    }
    has_stream = message->has_stream ||
        (message->stream.flags & (NB_STREAM_FIRST | NB_STREAM_LAST | NB_STREAM_CANCEL)) != 0 ||
        message->stream.stream_id != 0;
    memset(&msg, 0, sizeof(msg));
    msg.kind = message->kind;
    msg.flags = message->flags;
    msg.status = message->status;
    msg.request_id = message->request_id;
    msg.timeout_ms = message->timeout_ms;
    msg.metadata = message->metadata;
    msg.metadata_len = message->metadata_len;
    msg.status_msg = message->status_msg;
    msg.status_msg_len = message->status_msg_len;
    msg.route = message->route;
    msg.payload = message->payload;
    if (has_stream) {
        msg.stream = message->stream;
        msg.has_stream = 1;
    }
    msg.peer_ip = peer_host;
    msg.peer_port = peer_port;
    msg.peer_token = reply_token;
    nb_node_on_message(transport->node, transport, &msg);
}

/*
 * provider 适配层：把外部 provider SPI 适配成统一的 nb_transport_ops_t，
 * 使 node/upper 层只面对单一传输接口（不再有 provider/ops 双分支）。
 */
static int nb_provider_op_start(nb_transport_t *t)
{
    return t->provider->ops->start(t->provider_instance);
}

static void nb_provider_op_stop(nb_transport_t *t)
{
    t->provider->ops->stop(t->provider_instance);
}

/* 把统一消息上下文转换成公开 provider SPI 的消息结构。 */
static void nb_provider_message_from(const nb_message_t *src, nb_transport_message_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->kind = src->kind;
    dst->flags = src->flags;
    dst->status = src->status;
    dst->request_id = src->request_id;
    dst->timeout_ms = src->timeout_ms;
    dst->metadata = src->metadata;
    dst->metadata_len = src->metadata_len;
    dst->status_msg = src->status_msg;
    dst->status_msg_len = src->status_msg_len;
    dst->has_stream = src->has_stream;
    dst->stream = src->stream;
    dst->route = src->route;
    dst->payload = src->payload;
}

static int nb_provider_op_send(nb_transport_t *t, const nb_message_t *message)
{
    nb_transport_message_t provider_message;
    if (!t || !t->provider || !message) {
        return NB_ERR_INVALID;
    }
    nb_provider_message_from(message, &provider_message);
    return t->provider->ops->send(
        t->provider_instance, message->peer_ip, message->peer_port, &provider_message);
}

static int nb_provider_op_reply(nb_transport_t *t, const nb_message_t *message)
{
    nb_transport_message_t provider_message;
    if (!t || !t->provider || !message) {
        return NB_ERR_INVALID;
    }
    nb_provider_message_from(message, &provider_message);
    return t->provider->ops->reply(t->provider_instance,
                                   message->peer_token,
                                   message->peer_ip,
                                   message->peer_port,
                                   &provider_message);
}

static int nb_provider_op_peer_stats(nb_transport_t *t,
                                  const char *ip,
                                  uint16_t port,
                                  nb_peer_stats_t *out)
{
    (void)t;
    (void)ip;
    (void)port;
    (void)out;
    return 0;
}

static const nb_transport_ops_t nb_g_provider_ops = {
    nb_provider_op_start, nb_provider_op_stop,       nb_provider_op_send,
    nb_provider_op_reply, nb_provider_op_peer_stats,
};

int nb_node_transport_add(nb_node_t *node,
                          const char *scheme,
                          const char *host,
                          uint16_t port,
                          nb_intra_process_t intra_process)
{
    nb_transport_t *transport;
    nb_transport_kind_t kind;
    const nb_transport_provider_t *provider;
    char endpoint[NB_ENDPOINT_MAX + NB_SCHEME_MAX + 16];
    if (!node || !scheme || !host || !host[0] || node->transport_count >= NB_MAX_BINDINGS) {
        return NB_ERR_INVALID;
    }
    kind = nb_transport_kind_from_scheme(scheme);
    provider = nb_transport_provider_find(scheme);
    if (kind != NB_TRANSPORT_TCP && kind != NB_TRANSPORT_UDP && !provider) {
        return NB_ERR_NOTFOUND; /* 未注册的 scheme */
    }
    transport =
        provider ? (nb_transport_t *)calloc(1, sizeof(*transport)) : nb_create_transport(kind);
    if (!transport) {
        return NB_ERR_NOMEM;
    }
    transport->node = node;
    nb_copy_str(transport->scheme, sizeof(transport->scheme), scheme, "udp");
    nb_copy_str(transport->bind_host, sizeof(transport->bind_host), host, "0.0.0.0");
    transport->port = port;
    /* 传输层只读的运行期配置快照（见 nb_transport.h）。 */
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
        nb_transport_provider_options_t options;
        nb_transport_instance_t *instance = NULL;
        int n = snprintf(endpoint, sizeof(endpoint), "%s://%s:%u", scheme, host, (unsigned)port);
        if (n < 0 || (size_t)n >= sizeof(endpoint)) {
            free(transport);
            return NB_ERR_TOOBIG;
        }
        transport->kind = NB_TRANSPORT_CUSTOM;
        transport->ops = &nb_g_provider_ops;
        transport->provider = provider;
        memset(&options, 0, sizeof(options));
        options.scheme = scheme;
        options.endpoint = endpoint;
        options.receiver = transport;
        options.on_receive = nb_provider_on_receive;
        options.intra_process = intra_process;
        if (provider->ops->create(&options, &instance) != NB_OK || !instance) {
            free(transport);
            return NB_ERR_IO;
        }
        transport->provider_instance = instance;
    }
    if (nb_transport_start(transport) != NB_OK) {
        nb_transport_destroy(transport);
        return NB_ERR_IO;
    }
    node->transports[node->transport_count++] = transport;
    return NB_OK;
}
