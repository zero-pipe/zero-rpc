#include "zrpc_envelope.h"
#include "zrpc_wire.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

static void test_envelope_roundtrip(void) {
    uint8_t buf[256];
    const char *route = "echo.ping";
    const uint8_t payload[5] = {1, 2, 3, 4, 5};
    zrpc_envelope_t envelope;
    zrpc_envelope_view_t view;
    size_t n;

    memset(&envelope, 0, sizeof(envelope));
    envelope.kind = ZRPC_KIND_REQUEST;
    envelope.encoding = 3;
    envelope.request_id = 42;
    envelope.stream_flags = ZRPC_STREAM_FLAG_STREAM | ZRPC_STREAM_FLAG_FIRST;
    envelope.stream_id = 99;
    envelope.stream_offset = 0x1122334455667788ull;
    envelope.stream_total_size = 0x0a0b0c0d0e0f1011ull;
    envelope.route = route;
    envelope.payload = payload;
    envelope.payload_len = sizeof(payload);
    n = zrpc_envelope_encode(buf, sizeof(buf), &envelope);
    CHECK(n == zrpc_envelope_size(strlen(route), sizeof(payload)));
    CHECK(zrpc_envelope_decode(buf, n, &view) == ZRPC_OK);
    CHECK(view.kind == ZRPC_KIND_REQUEST);
    CHECK(view.encoding == 3);
    CHECK(view.request_id == 42);
    CHECK(view.stream_flags == (ZRPC_STREAM_FLAG_STREAM | ZRPC_STREAM_FLAG_FIRST));
    CHECK(view.stream_id == 99);
    CHECK(view.stream_offset == 0x1122334455667788ull);
    CHECK(view.stream_total_size == 0x0a0b0c0d0e0f1011ull);
    CHECK(view.route_len == strlen(route));
    CHECK(memcmp(view.route, route, strlen(route)) == 0);
    CHECK(view.payload_len == sizeof(payload));
    CHECK(memcmp(view.payload, payload, sizeof(payload)) == 0);
}

static void test_udp_fragment_roundtrip(void) {
    uint8_t buf[256];
    const uint8_t chunk[5] = {1, 2, 3, 4, 5};
    zrpc_frag_desc_t desc;
    zrpc_frag_view_t view;
    size_t n;

    memset(&desc, 0, sizeof(desc));
    desc.marker = 1;
    desc.pt = ZRPC_PT_DATA;
    desc.seq = 0x1234;
    desc.ts = 0xdeadbeefu;
    desc.ssrc = 0x0a0b0c0du;
    desc.msg_id = 42;
    desc.total_len = sizeof(chunk);
    desc.frag_len = sizeof(chunk);
    desc.flags = ZRPC_FLAG_FIRST | ZRPC_FLAG_LAST;
    desc.chunk = chunk;
    n = zrpc_wire_build_data(buf, sizeof(buf), &desc);
    CHECK(n > 0);
    CHECK(zrpc_wire_parse_data(buf, n, &view) == 1);
    CHECK(view.marker == 1);
    CHECK(view.seq == 0x1234);
    CHECK(view.ssrc == 0x0a0b0c0du);
    CHECK(view.msg_id == 42);
    CHECK(view.total_len == sizeof(chunk));
    CHECK(view.frag_len == sizeof(chunk));
    CHECK(view.flags == (ZRPC_FLAG_FIRST | ZRPC_FLAG_LAST));
    CHECK(memcmp(view.chunk, chunk, sizeof(chunk)) == 0);
}

static void test_tcp_opaque_frame(void) {
    uint8_t buf[64];
    const uint8_t body[4] = {9, 8, 7, 6};
    const uint8_t *parsed = NULL;
    size_t parsed_len = 0;
    size_t n;

    n = zrpc_tcp_build(buf, sizeof(buf), body, sizeof(body));
    CHECK(n == sizeof(body) + ZRPC_TCP_HDR_LEN);
    CHECK(zrpc_tcp_parse(buf, n, &parsed, &parsed_len) == 1);
    CHECK(parsed_len == sizeof(body));
    CHECK(memcmp(parsed, body, sizeof(body)) == 0);
}

static void test_nack_roundtrip(void) {
    uint8_t buf[128];
    uint16_t seqs[4] = {100, 101, 103, 200};
    uint16_t out[64];
    size_t n;
    int c;

    n = zrpc_wire_build_nack(buf, sizeof(buf), 0x11111111u, 0x22222222u, seqs, 4);
    CHECK(n > 0);
    c = zrpc_wire_parse_nack(buf, n, NULL, NULL, out, 64);
    CHECK(c == 4);
    CHECK(out[0] == 100);
    CHECK(out[1] == 101);
    CHECK(out[2] == 103);
    CHECK(out[3] == 200);
}

static void test_parse_rejects_rtcp(void) {
    uint8_t rtcp[12];
    zrpc_frag_view_t view;
    memset(rtcp, 0, sizeof(rtcp));
    rtcp[0] = (uint8_t)(2u << 6);
    rtcp[1] = ZRPC_RTCP_RTPFB;
    CHECK(zrpc_wire_parse_data(rtcp, sizeof(rtcp), &view) == 0);
}

int main(void) {
    test_envelope_roundtrip();
    test_udp_fragment_roundtrip();
    test_tcp_opaque_frame();
    test_nack_roundtrip();
    test_parse_rejects_rtcp();
    if (g_fail) {
        printf("zrpc_wire: FAIL\n");
        return 1;
    }
    printf("zrpc_wire: OK\n");
    return 0;
}
