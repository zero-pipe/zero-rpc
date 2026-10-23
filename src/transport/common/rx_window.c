#include "rx_window.h"

#include <string.h>

static int nb_seq_diff16(uint16_t seq, uint16_t base)
{
    return (int)(int16_t)(uint16_t)(seq - base);
}

static int nb_bit_test(const nb_rx_window_t *window, int index)
{
    if (index < 0 || index >= WINDOW_BITS) {
        return 0;
    }
    return (int)((window->bits[index >> 6] >> (index & 63)) & 1u);
}

static void nb_bit_set(nb_rx_window_t *window, int index)
{
    if (index >= 0 && index < WINDOW_BITS) {
        window->bits[index >> 6] |= (1ull << (index & 63));
    }
}

static void nb_window_shift(nb_rx_window_t *window)
{
    int i;
    for (i = 0; i < WINDOW_WORDS - 1; i++) {
        window->bits[i] = (window->bits[i] >> 1) | (window->bits[i + 1] << 63);
    }
    window->bits[WINDOW_WORDS - 1] >>= 1;
}

void nb_rx_window_reset(nb_rx_window_t *window)
{
    if (window) {
        memset(window, 0, sizeof(*window));
    }
}

int nb_rx_window_accept(nb_rx_window_t *window, uint16_t seq)
{
    int diff;
    if (!window) {
        return 0;
    }
    if (!window->initialized) {
        window->initialized = 1;
        window->base = seq;
        window->max = seq;
    }
    diff = nb_seq_diff16(seq, window->base);
    if (diff < 0) {
        return 0;
    }
    if (diff >= WINDOW_BITS) {
        memset(window->bits, 0, sizeof(window->bits));
        window->base = seq;
        window->max = seq;
        diff = 0;
    }
    if (nb_bit_test(window, diff)) {
        return 0; /* duplicate within the window: do not redeliver */
    }
    nb_bit_set(window, diff);
    if (nb_seq_diff16(seq, window->max) > 0) {
        window->max = seq;
    }
    while (nb_bit_test(window, 0)) {
        nb_window_shift(window);
        window->base++;
    }
    return 1;
}

size_t nb_rx_window_missing(const nb_rx_window_t *window, uint16_t *out, size_t cap)
{
    int span;
    int limit;
    int i;
    size_t count = 0;
    if (!window || !window->initialized || !out || cap == 0) {
        return 0;
    }
    span = nb_seq_diff16(window->max, window->base);
    limit = span;
    if (limit >= WINDOW_BITS) {
        limit = WINDOW_BITS - 1;
    }
    for (i = 0; i < limit && count < cap; i++) {
        if (!nb_bit_test(window, i)) {
            out[count++] = (uint16_t)(window->base + i);
        }
    }
    return count;
}
