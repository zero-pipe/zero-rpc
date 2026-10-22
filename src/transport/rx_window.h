#ifndef RX_WINDOW_H
#define RX_WINDOW_H

#include <stddef.h>
#include <stdint.h>

#ifndef WINDOW_BITS
#define WINDOW_BITS 1024
#endif

#define WINDOW_WORDS (WINDOW_BITS / 64)

/* Wrap-safe receive sequence window used by unreliable datagram providers. */
typedef struct nb_rx_window {
    int initialized;
    uint16_t base;
    uint16_t max;
    uint64_t bits[WINDOW_WORDS];
} nb_rx_window_t;

void nb_rx_window_reset(nb_rx_window_t *window);
/* Returns 1 for a new sequence, 0 for a duplicate (old or still in-window). */
int nb_rx_window_accept(nb_rx_window_t *window, uint16_t seq);
size_t nb_rx_window_missing(const nb_rx_window_t *window, uint16_t *out, size_t cap);

#endif /* RX_WINDOW_H */
