#include "zrpc_rx_window.h"

#include <string.h>

static int seq_diff16(uint16_t seq, uint16_t base) {
    return (int)(int16_t)(uint16_t)(seq - base);
}

static int bit_test(const zrpc_rx_window_t *window, int index) {
    if (index < 0 || index >= ZRPC_WINDOW_BITS) {
        return 0;
    }
    return (int)((window->bits[index >> 6] >> (index & 63)) & 1u);
}

static void bit_set(zrpc_rx_window_t *window, int index) {
    if (index >= 0 && index < ZRPC_WINDOW_BITS) {
        window->bits[index >> 6] |= (1ull << (index & 63));
    }
}

static void window_shift(zrpc_rx_window_t *window) {
    int i;
    for (i = 0; i < ZRPC_WINDOW_WORDS - 1; i++) {
        window->bits[i] = (window->bits[i] >> 1) | (window->bits[i + 1] << 63);
    }
    window->bits[ZRPC_WINDOW_WORDS - 1] >>= 1;
}

void zrpc_rx_window_reset(zrpc_rx_window_t *window) {
    if (window) {
        memset(window, 0, sizeof(*window));
    }
}

int zrpc_rx_window_accept(zrpc_rx_window_t *window, uint16_t seq) {
    int diff;
    if (!window) {
        return 0;
    }
    if (!window->initialized) {
        window->initialized = 1;
        window->base = seq;
        window->max = seq;
    }
    diff = seq_diff16(seq, window->base);
    if (diff < 0) {
        return 0;
    }
    if (diff >= ZRPC_WINDOW_BITS) {
        memset(window->bits, 0, sizeof(window->bits));
        window->base = seq;
        window->max = seq;
        diff = 0;
    }
    bit_set(window, diff);
    if (seq_diff16(seq, window->max) > 0) {
        window->max = seq;
    }
    while (bit_test(window, 0)) {
        window_shift(window);
        window->base++;
    }
    return 1;
}

size_t zrpc_rx_window_missing(const zrpc_rx_window_t *window, uint16_t *out, size_t cap) {
    int span;
    int limit;
    int i;
    size_t count = 0;
    if (!window || !window->initialized || !out || cap == 0) {
        return 0;
    }
    span = seq_diff16(window->max, window->base);
    limit = span;
    if (limit >= ZRPC_WINDOW_BITS) {
        limit = ZRPC_WINDOW_BITS - 1;
    }
    for (i = 0; i < limit && count < cap; i++) {
        if (!bit_test(window, i)) {
            out[count++] = (uint16_t)(window->base + i);
        }
    }
    return count;
}
