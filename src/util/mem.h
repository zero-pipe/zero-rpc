#ifndef MEM_H
#define MEM_H

#include <stddef.h>

/* Poller-local pooled allocation with safe cross-thread fallback release. */
void *nb_mem_alloc(size_t size);
void nb_mem_free(void *ptr);

#endif /* MEM_H */
