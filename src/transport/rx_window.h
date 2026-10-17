#ifndef RX_WINDOW_H
#define RX_WINDOW_H

#include <stddef.h>
#include <stdint.h>

#ifndef WINDOW_BITS
#define WINDOW_BITS 1024
#endif

#define WINDOW_WORDS (WINDOW_BITS / 64)

/* Wrap-safe receive sequence window used by unreliable datagram providers. */
typedef struct zri_rx_window {
    int initialized;
    uint16_t base;
    uint16_t max;
    uint64_t bits[WINDOW_WORDS];
} zri_rx_window_t;

void zri_rx_window_reset(zri_rx_window_t *window);
/* Returns 1 for a sequence accepted into the window, 0 for an old duplicate. */
int zri_rx_window_accept(zri_rx_window_t *window, uint16_t seq);
size_t zri_rx_window_missing(const zri_rx_window_t *window, uint16_t *out, size_t cap);

#endif /* RX_WINDOW_H */
