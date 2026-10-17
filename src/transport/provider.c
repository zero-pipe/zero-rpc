#include "transport_internal.h"

#include <string.h>

#define PROVIDER_MAX 32

static const transport_provider_t *g_providers[PROVIDER_MAX];
static size_t g_provider_count;

/* ---- Provider 注册 ---- */

int transport_provider_register(const transport_provider_t *provider) {
    size_t i;
    if (!provider || !provider->scheme || !provider->scheme[0] || !provider->ops ||
        !provider->ops->create || !provider->ops->start || !provider->ops->stop ||
        !provider->ops->destroy || !provider->ops->send || !provider->ops->reply) {
        return ERR_INVALID;
    }
    for (i = 0; i < g_provider_count; i++) {
        if (strcmp(g_providers[i]->scheme, provider->scheme) == 0) {
            return ERR_STATE;
        }
    }
    if (g_provider_count >= PROVIDER_MAX) {
        return ERR_NOMEM;
    }
    g_providers[g_provider_count++] = provider;
    return OK;
}

const transport_provider_t *zri_transport_provider_find(const char *scheme) {
    size_t i;
    if (!scheme) {
        return NULL;
    }
    for (i = 0; i < g_provider_count; i++) {
        if (strcmp(g_providers[i]->scheme, scheme) == 0) {
            return g_providers[i];
        }
    }
    return NULL;
}
