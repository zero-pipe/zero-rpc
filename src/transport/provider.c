#include "transport_internal.h"

#include <string.h>

#define PROVIDER_MAX 32

static const nb_transport_provider_t *nb_g_providers[PROVIDER_MAX];
static size_t nb_g_provider_count;

/* ---- Provider 注册 ---- */

int nb_transport_provider_register(const nb_transport_provider_t *provider)
{
    size_t i;
    if (!provider || !provider->scheme || !provider->scheme[0] || !provider->ops ||
        !provider->ops->create || !provider->ops->start || !provider->ops->stop ||
        !provider->ops->destroy || !provider->ops->send || !provider->ops->reply) {
        return NB_ERR_INVALID;
    }
    for (i = 0; i < nb_g_provider_count; i++) {
        if (strcmp(nb_g_providers[i]->scheme, provider->scheme) == 0) {
            return NB_ERR_STATE;
        }
    }
    if (nb_g_provider_count >= PROVIDER_MAX) {
        return NB_ERR_NOMEM;
    }
    nb_g_providers[nb_g_provider_count++] = provider;
    return NB_OK;
}

const nb_transport_provider_t *nb_transport_provider_find(const char *scheme)
{
    size_t i;
    if (!scheme) {
        return NULL;
    }
    for (i = 0; i < nb_g_provider_count; i++) {
        if (strcmp(nb_g_providers[i]->scheme, scheme) == 0) {
            return nb_g_providers[i];
        }
    }
    return NULL;
}
