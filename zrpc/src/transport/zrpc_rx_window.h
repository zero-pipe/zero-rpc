#ifndef ZRPC_RX_WINDOW_H
#define ZRPC_RX_WINDOW_H

#include <stddef.h>
#include <stdint.h>

#ifndef ZRPC_WINDOW_BITS
#define ZRPC_WINDOW_BITS 1024
#endif

#define ZRPC_WINDOW_WORDS (ZRPC_WINDOW_BITS / 64)

/* Wrap-safe receive sequence window used by unreliable datagram providers. */
typedef struct zrpc_rx_window {
    int initialized;
    uint16_t base;
    uint16_t max;
    uint64_t bits[ZRPC_WINDOW_WORDS];
} zrpc_rx_window_t;

void zrpc_rx_window_reset(zrpc_rx_window_t *window);
/* Returns 1 for a sequence accepted into the window, 0 for an old duplicate. */
int zrpc_rx_window_accept(zrpc_rx_window_t *window, uint16_t seq);
size_t zrpc_rx_window_missing(const zrpc_rx_window_t *window, uint16_t *out, size_t cap);

#endif /* ZRPC_RX_WINDOW_H */
