#include "zrpc_transport.h"

#include <string.h>

#define ZRPC_PROVIDER_MAX 32

static const zrpc_transport_provider_t *g_providers[ZRPC_PROVIDER_MAX];
static size_t g_provider_count;

int zrpc_transport_provider_register(const zrpc_transport_provider_t *provider) {
    size_t i;
    if (!provider || !provider->scheme || !provider->scheme[0] || !provider->ops ||
        !provider->ops->create || !provider->ops->start || !provider->ops->stop ||
        !provider->ops->destroy || !provider->ops->send || !provider->ops->reply) {
        return ZRPC_ERR_INVALID;
    }
    for (i = 0; i < g_provider_count; i++) {
        if (strcmp(g_providers[i]->scheme, provider->scheme) == 0) {
            return ZRPC_ERR_STATE;
        }
    }
    if (g_provider_count >= ZRPC_PROVIDER_MAX) {
        return ZRPC_ERR_NOMEM;
    }
    g_providers[g_provider_count++] = provider;
    return ZRPC_OK;
}

const zrpc_transport_provider_t *zrpc_transport_provider_find(const char *scheme) {
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
