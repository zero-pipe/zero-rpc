#ifndef ZRPC_REGISTRY_BACKEND_H
#define ZRPC_REGISTRY_BACKEND_H

#include <zrpc/zrpc_types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zrpc_node zrpc_node_t;

/*
 * Control-plane backend lifecycle. A backend keeps the node registry in sync
 * with external peers (mesh broadcast, KV watch, static file, ...). It must
 * never touch data-plane framing or service handlers.
 */
typedef struct zrpc_registry_backend_ops {
    /* Start listening/timers. Return ZRPC_OK on success. */
    int (*start)(zrpc_node_t *node);
    void (*stop)(zrpc_node_t *node);
    /* Periodic maintenance: lease expiry, re-announce, watch reconnect. */
    void (*tick)(zrpc_node_t *node, uint64_t now_ms);
    /* Announce the node's current services/bindings. */
    void (*announce)(zrpc_node_t *node, int bye);
} zrpc_registry_backend_ops_t;

typedef struct zrpc_registry_backend {
    const char *name;
    const zrpc_registry_backend_ops_t *ops;
} zrpc_registry_backend_t;

/* Mesh UDP broadcast backend (built-in). */
const zrpc_registry_backend_t *zrpc_registry_backend_mesh(void);

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_REGISTRY_BACKEND_H */
