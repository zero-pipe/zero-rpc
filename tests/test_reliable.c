/* 可复用可靠性模块：接收窗口、重组、重传环。 */
#include "rx_window.h"
#include "reasm.h"
#include "tx_ring.h"

#define NB_LOG_TAG "test"
#include <log.h>

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            nb_log_error("FAIL %s:%d: %s", __FILE__, __LINE__, #cond); \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

static void test_rx_window(void)
{
    nb_rx_window_t window;
    uint16_t missing[64];
    size_t n;

    nb_rx_window_reset(&window);
    CHECK(nb_rx_window_accept(&window, 10) == 1);
    CHECK(nb_rx_window_accept(&window, 12) == 1); /* gap at 11 */
    CHECK(nb_rx_window_accept(&window, 12) == 0); /* in-window duplicate rejected */
    n = nb_rx_window_missing(&window, missing, 64);
    CHECK(n == 1);
    CHECK(missing[0] == 11);

    CHECK(nb_rx_window_accept(&window, 11) == 1); /* fills gap, advances base */
    CHECK(nb_rx_window_accept(&window, 11) == 0); /* duplicate below base */
    n = nb_rx_window_missing(&window, missing, 64);
    CHECK(n == 0);

    /* Old sequence below base is rejected. */
    CHECK(nb_rx_window_accept(&window, 9) == 0);
}

static void test_reasm(void)
{
    nb_reasm_t reasm;
    nb_reasm_slot_t *slot;
    uint16_t missing[64];
    size_t n;
    const uint8_t data[4] = { 1, 2, 3, 4 };

    nb_reasm_init(&reasm);
    slot = nb_reasm_acquire(&reasm, 77, 8, 2, 1000);
    CHECK(slot != NULL);
    slot->base_seq_set = 1;
    slot->base_seq = 5;
    n = nb_reasm_missing(&reasm, missing, 64);
    CHECK(n == 2);
    CHECK(missing[0] == 5);
    CHECK(missing[1] == 6);

    CHECK(nb_reasm_put(slot, 0, 0, data, 4) == 1);
    CHECK(nb_reasm_put(slot, 0, 0, data, 4) == 0); /* duplicate */
    CHECK(nb_reasm_put(slot, 1, 4, data, 4) == 1);
    CHECK(slot->got == 2);
    CHECK(memcmp(slot->buf, data, 4) == 0);
    CHECK(memcmp(slot->buf + 4, data, 4) == 0);
    n = nb_reasm_missing(&reasm, missing, 64);
    CHECK(n == 0);

    /* Expiry releases the slot and reports it. */
    nb_reasm_expire(&reasm, 1000 + 5000, 3000, NULL, NULL);
    CHECK(nb_reasm_find(&reasm, 77) == NULL);
    nb_reasm_reset(&reasm);
}

static void test_tx_ring(void)
{
    nb_tx_ring_t ring;
    const uint8_t *packet;
    uint8_t *dst;
    uint32_t len = 0;

    memset(&ring, 0, sizeof(ring));
    CHECK(nb_tx_ring_ensure(&ring, 32) == 1);
    dst = nb_tx_ring_slot(&ring, 3);
    CHECK(dst != NULL);
    memcpy(dst, "abcdef", 6);
    nb_tx_ring_commit(&ring, 3, 6);
    packet = nb_tx_ring_get(&ring, 3, &len);
    CHECK(packet != NULL);
    CHECK(len == 6);
    CHECK(memcmp(packet, "abcdef", 6) == 0);

    /* Evicted sequence is no longer retrievable. */
    nb_tx_ring_commit(&ring, (uint16_t)(3 + RTX_RING), 6);
    CHECK(nb_tx_ring_get(&ring, 3, &len) == NULL);
    nb_tx_ring_cleanup(&ring);
}

static void test_reasm_budget(void)
{
    nb_reasm_t reasm;
    nb_reasm_init(&reasm);
    nb_reasm_set_budget(&reasm, 10);
    CHECK(nb_reasm_acquire(&reasm, 1, 8, 2, 0) != NULL);
    CHECK(nb_reasm_acquire(&reasm, 2, 8, 2, 0) == NULL); /* 8 + 8 > 10 */
    nb_reasm_reset(&reasm);
    CHECK(nb_reasm_acquire(&reasm, 3, 8, 2, 0) != NULL); /* reset 归还预算 */
    nb_reasm_reset(&reasm);
}

int main(void)
{
    test_rx_window();
    test_reasm();
    test_reasm_budget();
    test_tx_ring();
    if (g_fail) {
        nb_log_error("nb_reliable: FAIL");
        return 1;
    }
    nb_log_info("nb_reliable: NB_OK");
    return 0;
}
