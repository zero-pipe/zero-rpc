#include <gtest/gtest.h>

#include <cstring>

extern "C" {
#include "envelope.h"
#include "reasm.h"
#include "rx_window.h"
#include "tx_ring.h"
}

TEST(Envelope, RoundTripPreservesStreamMetadata) {
    const char route[] = "upload.chunk";
    const char payload[] = "payload";
    zri_envelope_t input = {};
    uint8_t buffer[256] = {};
    zri_envelope_t output = {};

    input.kind = ZRPC_KIND_REQUEST;
    input.encoding = 7;
    input.status = -3;
    input.stream_flags = ZRPC_STREAM_FLAG_STREAM | ZRPC_STREAM_FLAG_FIRST;
    input.request_id = 41;
    input.stream_id = 9;
    input.stream_offset = 123;
    input.stream_total_size = 456;
    input.route = route;
    input.payload = payload;
    input.payload_len = sizeof(payload) - 1;

    ASSERT_EQ(zri_envelope_encode(buffer, sizeof(buffer), &input),
              zri_envelope_size(strlen(route), sizeof(payload) - 1));
    ASSERT_EQ(zri_envelope_decode(buffer, zri_envelope_size(strlen(route), sizeof(payload) - 1),
                                   &output), ZRPC_OK);
    EXPECT_EQ(output.stream_flags, input.stream_flags);
    EXPECT_EQ(output.status, input.status);
    EXPECT_EQ(output.stream_id, input.stream_id);
    EXPECT_EQ(output.stream_offset, input.stream_offset);
    EXPECT_EQ(output.stream_total_size, input.stream_total_size);
    EXPECT_EQ(output.route_len, strlen(route));
    EXPECT_EQ(output.payload_len, sizeof(payload) - 1);
}

TEST(ReliableWindow, ReportsMissingSequence) {
    zri_rx_window_t window;
    uint16_t missing[8] = {};
    zri_rx_window_reset(&window);
    ASSERT_EQ(zri_rx_window_accept(&window, 100), 1);
    ASSERT_EQ(zri_rx_window_accept(&window, 102), 1);
    ASSERT_EQ(zri_rx_window_missing(&window, missing, 8), 1u);
    EXPECT_EQ(missing[0], 101);
}

TEST(Reassembly, RejectsOutOfBoundsFragment) {
    zri_reasm_t reasm;
    zri_reasm_init(&reasm);
    zri_reasm_slot_t *slot = zri_reasm_acquire(&reasm, 1, 8, 2, 0);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(zri_reasm_put(slot, 0, 7, reinterpret_cast<const uint8_t *>("xx"), 2), 0);
    zri_reasm_reset(&reasm);
}

TEST(TxRing, EvictsBySequenceModuloRing) {
    zri_tx_ring_t ring = {};
    ASSERT_EQ(zri_tx_ring_ensure(&ring, 32), 1);
    uint8_t *slot = zri_tx_ring_slot(&ring, 3);
    ASSERT_NE(slot, nullptr);
    memcpy(slot, "abc", 3);
    zri_tx_ring_commit(&ring, 3, 3);
    uint32_t len = 0;
    ASSERT_NE(zri_tx_ring_get(&ring, 3, &len), nullptr);
    EXPECT_EQ(len, 3u);
    zri_tx_ring_commit(&ring, static_cast<uint16_t>(3 + ZRPC_RTX_RING), 3);
    EXPECT_EQ(zri_tx_ring_get(&ring, 3, &len), nullptr);
    zri_tx_ring_cleanup(&ring);
}
