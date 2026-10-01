#ifndef ZRPC_ENDPOINT_H
#define ZRPC_ENDPOINT_H

#include <zrpc/zrpc_types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A transport-neutral endpoint. The scheme selects a transport provider. */
typedef struct zrpc_endpoint {
    char scheme[ZRPC_SCHEME_MAX];
    char host[ZRPC_ENDPOINT_MAX];
    uint16_t port;
    zrpc_transport_kind_t transport;
} zrpc_endpoint_t;

/* Parse scheme://host:port. The input is not retained. */
int zrpc_endpoint_parse(const char *uri, zrpc_endpoint_t *out);

/* Format an endpoint URI into out. */
int zrpc_endpoint_format(const zrpc_endpoint_t *endpoint, char *out, size_t cap);

/* Map a provider scheme to a built-in transport kind, if any. */
zrpc_transport_kind_t zrpc_transport_kind_from_scheme(const char *scheme);

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_ENDPOINT_H */
