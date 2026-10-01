/* 可复用可靠性模块：接收窗口、重组、重传环。 */
#include "zrpc_rx_window.h"
#include "zrpc_reasm.h"
#include "zrpc_tx_ring.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail = 1;                                            \
        }                                                          \
    } while (0)

static void test_rx_window(void) {
    zrpc_rx_window_t window;
    uint16_t missing[64];
    size_t n;

    zrpc_rx_window_reset(&window);
    CHECK(zrpc_rx_window_accept(&window, 10) == 1);
    CHECK(zrpc_rx_window_accept(&window, 12) == 1); /* gap at 11 */
    CHECK(zrpc_rx_window_accept(&window, 12) == 1); /* duplicate still accepted */
    n = zrpc_rx_window_missing(&window, missing, 64);
    CHECK(n == 1);
    CHECK(missing[0] == 11);

    CHECK(zrpc_rx_window_accept(&window, 11) == 1); /* fills gap, advances base */
    n = zrpc_rx_window_missing(&window, missing, 64);
    CHECK(n == 0);

    /* Old sequence below base is rejected. */
    CHECK(zrpc_rx_window_accept(&window, 9) == 0);
}

static void test_reasm(void) {
    zrpc_reasm_t reasm;
    zrpc_reasm_slot_t *slot;
    uint16_t missing[64];
    size_t n;
    const uint8_t data[4] = {1, 2, 3, 4};

    zrpc_reasm_init(&reasm);
    slot = zrpc_reasm_acquire(&reasm, 77, 8, 2, 1000);
    CHECK(slot != NULL);
    slot->base_seq_set = 1;
    slot->base_seq = 5;
    n = zrpc_reasm_missing(&reasm, missing, 64);
    CHECK(n == 2);
    CHECK(missing[0] == 5);
    CHECK(missing[1] == 6);

    CHECK(zrpc_reasm_put(slot, 0, 0, data, 4) == 1);
    CHECK(zrpc_reasm_put(slot, 0, 0, data, 4) == 0); /* duplicate */
    CHECK(zrpc_reasm_put(slot, 1, 4, data, 4) == 1);
    CHECK(slot->got == 2);
    CHECK(memcmp(slot->buf, data, 4) == 0);
    CHECK(memcmp(slot->buf + 4, data, 4) == 0);
    n = zrpc_reasm_missing(&reasm, missing, 64);
    CHECK(n == 0);

    /* Expiry releases the slot and reports it. */
    zrpc_reasm_expire(&reasm, 1000 + 5000, 3000, NULL, NULL);
    CHECK(zrpc_reasm_find(&reasm, 77) == NULL);
    zrpc_reasm_reset(&reasm);
}

static void test_tx_ring(void) {
    zrpc_tx_ring_t ring;
    const uint8_t *packet;
    uint8_t *dst;
    uint32_t len = 0;

    memset(&ring, 0, sizeof(ring));
    CHECK(zrpc_tx_ring_ensure(&ring, 32) == 1);
    dst = zrpc_tx_ring_slot(&ring, 3);
    CHECK(dst != NULL);
    memcpy(dst, "abcdef", 6);
    zrpc_tx_ring_commit(&ring, 3, 6);
    packet = zrpc_tx_ring_get(&ring, 3, &len);
    CHECK(packet != NULL);
    CHECK(len == 6);
    CHECK(memcmp(packet, "abcdef", 6) == 0);

    /* Evicted sequence is no longer retrievable. */
    zrpc_tx_ring_commit(&ring, (uint16_t)(3 + ZRPC_RTX_RING), 6);
    CHECK(zrpc_tx_ring_get(&ring, 3, &len) == NULL);
    zrpc_tx_ring_cleanup(&ring);
}

int main(void) {
    test_rx_window();
    test_reasm();
    test_tx_ring();
    if (g_fail) {
        printf("zrpc_reliable: FAIL\n");
        return 1;
    }
    printf("zrpc_reliable: OK\n");
    return 0;
}
