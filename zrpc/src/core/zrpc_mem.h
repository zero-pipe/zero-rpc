#ifndef ZRPC_MEM_H
#define ZRPC_MEM_H

#include <stddef.h>

/* Poller-local pooled allocation with safe cross-thread fallback release. */
void *zrpc_mem_alloc(size_t size);
void zrpc_mem_free(void *ptr);

#endif /* ZRPC_MEM_H */
