#include "envelope.h"
#include "framing.h"

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
    zri_envelope_t envelope;
    zri_envelope_t view;
    size_t n;

    memset(&envelope, 0, sizeof(envelope));
    envelope.kind = ZRPC_KIND_REQUEST;
    envelope.encoding = 3;
    envelope.status = -7;
    envelope.request_id = 42;
    envelope.stream_flags = ZRPC_STREAM_FLAG_STREAM | ZRPC_STREAM_FLAG_FIRST;
    envelope.stream_id = 99;
    envelope.stream_offset = 0x1122334455667788ull;
    envelope.stream_total_size = 0x0a0b0c0d0e0f1011ull;
    envelope.route = route;
    envelope.payload = payload;
    envelope.payload_len = sizeof(payload);
    n = zri_envelope_encode(buf, sizeof(buf), &envelope);
    CHECK(n == zri_envelope_size(strlen(route), sizeof(payload)));
    CHECK(zri_envelope_decode(buf, n, &view) == ZRPC_OK);
    CHECK(view.kind == ZRPC_KIND_REQUEST);
    CHECK(view.encoding == 3);
    CHECK(view.status == -7);
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
    zri_frag_t desc;
    zri_frag_t view;
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
    n = zri_frame_build_data(buf, sizeof(buf), &desc);
    CHECK(n > 0);
    CHECK(zri_frame_parse_data(buf, n, &view) == 1);
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

    n = zri_tcp_build(buf, sizeof(buf), body, sizeof(body));
    CHECK(n == sizeof(body) + ZRPC_TCP_HDR_LEN);
    CHECK(zri_tcp_parse(buf, n, &parsed, &parsed_len) == 1);
    CHECK(parsed_len == sizeof(body));
    CHECK(memcmp(parsed, body, sizeof(body)) == 0);
}

static void test_nack_roundtrip(void) {
    uint8_t buf[128];
    uint16_t seqs[4] = {100, 101, 103, 200};
    uint16_t out[64];
    size_t n;
    int c;

    n = zri_frame_build_nack(buf, sizeof(buf), 0x11111111u, 0x22222222u, seqs, 4);
    CHECK(n > 0);
    c = zri_frame_parse_nack(buf, n, NULL, NULL, out, 64);
    CHECK(c == 4);
    CHECK(out[0] == 100);
    CHECK(out[1] == 101);
    CHECK(out[2] == 103);
    CHECK(out[3] == 200);
}

static void test_parse_rejects_rtcp(void) {
    uint8_t rtcp[12];
    zri_frag_t view;
    memset(rtcp, 0, sizeof(rtcp));
    rtcp[0] = (uint8_t)(2u << 6);
    rtcp[1] = ZRPC_RTCP_RTPFB;
    CHECK(zri_frame_parse_data(rtcp, sizeof(rtcp), &view) == 0);
}

static void test_rtcp_sr_roundtrip(void) {
    uint8_t buf[256];
    zri_rtcp_report_block_t rb;
    zri_rtcp_sr_t sr;
    size_t n;

    memset(&rb, 0, sizeof(rb));
    rb.ssrc = 0x01020304u;
    rb.fraction_lost = 7;
    rb.cumulative_lost = -3;
    rb.ext_highest_seq = 0x0a0b0c0du;
    rb.jitter = 1234;
    rb.lsr = 0x11223344u;
    rb.dlsr = 0x55667788u;

    n = zri_frame_build_sr(buf, sizeof(buf), 0xdeadbeefu, 0x11111111u, 0x22222222u, 0x33333333u,
                            100, 2000, &rb, 1);
    CHECK(n == 52);
    memset(&sr, 0, sizeof(sr));
    CHECK(zri_frame_parse_sr(buf, n, &sr) == 1);
    CHECK(sr.ssrc == 0xdeadbeefu);
    CHECK(sr.ntp_msw == 0x11111111u && sr.ntp_lsw == 0x22222222u);
    CHECK(sr.rtp_ts == 0x33333333u);
    CHECK(sr.packet_count == 100 && sr.octet_count == 2000);
    CHECK(sr.rb_count == 1);
    CHECK(sr.rb[0].ssrc == 0x01020304u);
    CHECK(sr.rb[0].fraction_lost == 7);
    CHECK(sr.rb[0].cumulative_lost == -3);
    CHECK(sr.rb[0].ext_highest_seq == 0x0a0b0c0du);
    CHECK(sr.rb[0].jitter == 1234);
    CHECK(sr.rb[0].lsr == 0x11223344u);
    CHECK(sr.rb[0].dlsr == 0x55667788u);
}

static void test_rtcp_rr_roundtrip(void) {
    uint8_t buf[128];
    zri_rtcp_report_block_t rb;
    zri_rtcp_rr_t rr;
    size_t n;

    memset(&rb, 0, sizeof(rb));
    rb.ssrc = 0x01010101u;
    rb.cumulative_lost = 5;
    n = zri_frame_build_rr(buf, sizeof(buf), 0x0f0f0f0fu, &rb, 1);
    CHECK(n == 32);
    memset(&rr, 0, sizeof(rr));
    CHECK(zri_frame_parse_rr(buf, n, &rr) == 1);
    CHECK(rr.ssrc == 0x0f0f0f0fu);
    CHECK(rr.rb_count == 1);
    CHECK(rr.rb[0].ssrc == 0x01010101u);
    CHECK(rr.rb[0].cumulative_lost == 5);
}

static void test_rtcp_sdes_and_dispatch(void) {
    uint8_t buf[128];
    zri_frag_t view;
    size_t n;

    n = zri_frame_build_sdes_cname(buf, sizeof(buf), 0xabcdef01u, "zrpc");
    CHECK(n == 16); /* 4 头 + 4 ssrc + 2 项头 + 4 名 + 2 填充 */
    CHECK(zri_frame_parse_data(buf, n, &view) == 0); /* SDES 不是数据包 */

    memset(buf, 0, sizeof(buf));
    buf[0] = (uint8_t)(2u << 6);
    buf[1] = ZRPC_RTCP_SR;
    CHECK(zri_frame_parse_sr(buf, 3, NULL) == 0);
}

static void test_rtcp_bye_roundtrip(void) {
    uint8_t buf[16];
    uint32_t ssrc = 0;
    size_t n;

    n = zri_frame_build_bye(buf, sizeof(buf), 0x0badf00du);
    CHECK(n == 8);
    CHECK(zri_frame_parse_bye(buf, n, &ssrc) == 1);
    CHECK(ssrc == 0x0badf00du);
    CHECK(zri_frame_parse_bye(buf, 4, NULL) == 0);
}

int main(void) {
    test_envelope_roundtrip();
    test_udp_fragment_roundtrip();
    test_tcp_opaque_frame();
    test_nack_roundtrip();
    test_parse_rejects_rtcp();
    test_rtcp_sr_roundtrip();
    test_rtcp_rr_roundtrip();
    test_rtcp_sdes_and_dispatch();
    test_rtcp_bye_roundtrip();
    if (g_fail) {
        printf("zri_framing: FAIL\n");
        return 1;
    }
    printf("zri_framing: OK\n");
    return 0;
}
