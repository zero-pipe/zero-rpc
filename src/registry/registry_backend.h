#ifndef REGISTRY_BACKEND_H
#define REGISTRY_BACKEND_H

#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct node node_t;

/*
 * Control-plane backend lifecycle. A backend keeps the node registry in sync
 * with external peers (mesh broadcast, KV watch, static file, ...). It must
 * never touch data-plane framing or service handlers.
 */
typedef struct zri_registry_backend_ops {
    /* Start listening/timers. Return OK on success. */
    int (*start)(node_t *node);
    void (*stop)(node_t *node);
    /* Periodic maintenance: lease expiry, re-announce, watch reconnect. */
    void (*tick)(node_t *node, uint64_t now_ms);
    /* Announce the node's current services/bindings. */
    void (*announce)(node_t *node, int bye);
} zri_registry_backend_ops_t;

typedef struct zri_registry_backend {
    const char *name;
    const zri_registry_backend_ops_t *ops;
} zri_registry_backend_t;

/* Mesh UDP broadcast backend (built-in). */
const zri_registry_backend_t *zri_registry_backend_mesh(void);

#ifdef __cplusplus
}
#endif

#endif /* REGISTRY_BACKEND_H */
