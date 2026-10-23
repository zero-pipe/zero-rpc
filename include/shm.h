#ifndef NB_SHM_H
#define NB_SHM_H

/*
 * POSIX shared-memory transport provider (same host).
 *
 * Register the provider before creating any node, then use a SHM binding:
 *
 *   server: shm://<channel>:1
 *   client: shm://<channel>:2
 *
 * The client proxy endpoint must point at shm://<channel>:1. Both nodes must
 * use NB_MODE_STATIC. This first version supports one requester per channel;
 * use a distinct channel for each client/server pair.
 *
 * The provider carries the framework's encoded v2 envelope in a fixed-size
 * shared-memory ring. NB_SHM_MAX_MESSAGE_BYTES is the encoded envelope limit,
 * so the usable payload is slightly smaller.
 */

#include <transport_provider.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encoded envelope limit for one SHM slot. */
#define NB_SHM_MAX_MESSAGE_BYTES (64u * 1024u)

/* Idempotently register the built-in "shm" provider. */
int nb_shm_register(void);

/* Static provider descriptor, for callers that prefer explicit registration. */
const nb_transport_provider_t *nb_shm_provider(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_SHM_H */
