/* 传输抽象层的便捷包装。 */
#include "zrpc_transport.h"

#include <stdlib.h>

int zrpc_transport_start(zrpc_transport_t *t) {
    if (!t) {
        return ZRPC_ERR_INVALID;
    }
    if (t->provider) {
        return t->provider->ops->start(t->provider_instance);
    }
    if (!t->ops || !t->ops->start) {
        return ZRPC_ERR_INVALID;
    }
    return t->ops->start(t);
}

void zrpc_transport_stop(zrpc_transport_t *t) {
    if (t && t->provider) {
        t->provider->ops->stop(t->provider_instance);
    } else if (t && t->ops && t->ops->stop) {
        t->ops->stop(t);
    }
}

void zrpc_transport_destroy(zrpc_transport_t *t) {
    if (!t) {
        return;
    }
    zrpc_transport_stop(t);
    if (t->provider && t->provider_instance) {
        t->provider->ops->destroy(t->provider_instance);
        t->provider_instance = NULL;
    }
    free(t);
}

int zrpc_transport_send(zrpc_transport_t *t, const char *ip, uint16_t port, uint8_t kind,
                        uint32_t msg_id, const char *route, const zrpc_payload_t *payload,
                        const zrpc_stream_meta_t *stream) {
    if (!t) {
        return ZRPC_ERR_INVALID;
    }
    if (t->provider) {
        zrpc_transport_message_t message;
        message.kind = kind;
        message.request_id = msg_id;
        message.stream = stream ? *stream : (zrpc_stream_meta_t){0};
        message.route = route;
        message.payload = payload ? *payload : (zrpc_payload_t){0};
        return t->provider->ops->send(t->provider_instance, ip, port, &message);
    }
    return t->ops->send(t, ip, port, kind, msg_id, route, payload, stream);
}

int zrpc_transport_reply(zrpc_transport_t *t, zrpc_peer_token_t token, const char *ip,
                         uint16_t port, uint8_t kind, uint32_t msg_id, const char *route,
                         const zrpc_payload_t *payload, const zrpc_stream_meta_t *stream) {
    if (!t) {
        return ZRPC_ERR_INVALID;
    }
    if (t->provider) {
        zrpc_transport_message_t message;
        message.kind = kind;
        message.request_id = msg_id;
        message.stream = stream ? *stream : (zrpc_stream_meta_t){0};
        message.route = route;
        message.payload = payload ? *payload : (zrpc_payload_t){0};
        return t->provider->ops->reply(t->provider_instance, token, ip, port, &message);
    }
    return t->ops->reply(t, token, ip, port, kind, msg_id, route, payload, stream);
}
