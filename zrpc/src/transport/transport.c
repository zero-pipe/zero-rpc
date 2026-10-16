/* 传输抽象层的便捷包装。 */
#include "transport.h"

#include <stdlib.h>

int zri_transport_start(zri_transport_t *t) {
    if (!t || !t->ops || !t->ops->start) {
        return ZRPC_ERR_INVALID;
    }
    return t->ops->start(t);
}

void zri_transport_stop(zri_transport_t *t) {
    if (t && t->ops && t->ops->stop) {
        t->ops->stop(t);
    }
}

void zri_transport_destroy(zri_transport_t *t) {
    if (!t) {
        return;
    }
    zri_transport_stop(t);
    if (t->provider && t->provider_instance) {
        t->provider->ops->destroy(t->provider_instance);
        t->provider_instance = NULL;
    }
    free(t);
}

int zri_transport_send(zri_transport_t *t, const char *ip, uint16_t port, uint8_t kind,
                        int status, uint32_t msg_id, const char *route,
                        const zrpc_payload_t *payload, const zrpc_stream_meta_t *stream) {
    if (!t) {
        return ZRPC_ERR_INVALID;
    }
    return t->ops->send(t, ip, port, kind, status, msg_id, route, payload, stream);
}

int zri_transport_reply(zri_transport_t *t, zri_peer_token_t token, const char *ip,
                         uint16_t port, uint8_t kind, int status, uint32_t msg_id,
                         const char *route, const zrpc_payload_t *payload,
                         const zrpc_stream_meta_t *stream) {
    if (!t) {
        return ZRPC_ERR_INVALID;
    }
    return t->ops->reply(t, token, ip, port, kind, status, msg_id, route, payload, stream);
}
