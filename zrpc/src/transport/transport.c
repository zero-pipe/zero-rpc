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

int zri_transport_send(zri_transport_t *t, const zri_message_t *message) {
    if (!t || !t->ops || !t->ops->send || !message) {
        return ZRPC_ERR_INVALID;
    }
    return t->ops->send(t, message);
}

int zri_transport_reply(zri_transport_t *t, const zri_message_t *message) {
    if (!t || !t->ops || !t->ops->reply || !message) {
        return ZRPC_ERR_INVALID;
    }
    return t->ops->reply(t, message);
}
