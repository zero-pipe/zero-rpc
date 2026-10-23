/* 传输抽象层的便捷包装。 */
#include "transport_internal.h"

#include <stdlib.h>

int nb_transport_start(nb_transport_t *t)
{
    if (!t || !t->ops || !t->ops->start) {
        return NB_ERR_INVALID;
    }
    return t->ops->start(t);
}

void nb_transport_stop(nb_transport_t *t)
{
    if (t && t->ops && t->ops->stop) {
        t->ops->stop(t);
    }
}

void nb_transport_destroy(nb_transport_t *t)
{
    if (!t) {
        return;
    }
    nb_transport_stop(t);
    if (t->provider && t->provider_instance) {
        t->provider->ops->destroy(t->provider_instance);
        t->provider_instance = NULL;
    }
    free(t);
}

int nb_transport_send(nb_transport_t *t, const nb_message_t *message)
{
    if (!t || !t->ops || !t->ops->send || !message) {
        return NB_ERR_INVALID;
    }
    return t->ops->send(t, message);
}

int nb_transport_reply(nb_transport_t *t, const nb_message_t *message)
{
    if (!t || !t->ops || !t->ops->reply || !message) {
        return NB_ERR_INVALID;
    }
    return t->ops->reply(t, message);
}

int nb_transport_subscribe(nb_transport_t *t, const char *service, const char *method)
{
    if (!t || !service || !method) {
        return NB_ERR_INVALID;
    }
    if (!t->provider || !t->provider_instance || !t->provider->ops->subscribe) {
        return NB_OK;
    }
    return t->provider->ops->subscribe(t->provider_instance, service, method);
}

void nb_transport_unsubscribe(nb_transport_t *t, const char *service, const char *method)
{
    if (!t || !service || !method || !t->provider || !t->provider_instance ||
        !t->provider->ops->unsubscribe) {
        return;
    }
    t->provider->ops->unsubscribe(t->provider_instance, service, method);
}
