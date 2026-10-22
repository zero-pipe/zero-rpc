#ifndef REGISTRY_BACKEND_H
#define REGISTRY_BACKEND_H

#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nb_node nb_node_t;

/*
 * Control-plane backend lifecycle. A backend keeps the node registry in sync
 * with external peers (mesh broadcast, KV watch, static file, ...). It must
 * never touch data-plane framing or service handlers.
 */
typedef struct nb_registry_backend_ops {
    /* Start listening/timers. Return NB_OK on success. */
    int (*start)(nb_node_t *node);
    void (*stop)(nb_node_t *node);
    /* Periodic maintenance: lease expiry, re-announce, watch reconnect. */
    void (*tick)(nb_node_t *node, uint64_t now_ms);
    /* Announce the node's current services/bindings. */
    void (*announce)(nb_node_t *node, int bye);
} nb_registry_backend_ops_t;

typedef struct nb_registry_backend {
    const char *name;
    const nb_registry_backend_ops_t *ops;
} nb_registry_backend_t;

/* Mesh UDP broadcast backend (built-in). */
const nb_registry_backend_t *nb_registry_backend_mesh(void);

#ifdef __cplusplus
}
#endif

#endif /* REGISTRY_BACKEND_H */
