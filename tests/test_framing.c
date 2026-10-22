#include "envelope.h"
#include "framing.h"

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

static void test_envelope_roundtrip(void)
{
    uint8_t buf[256];
    const char *route = "echo.ping";
    const uint8_t payload[5] = { 1, 2, 3, 4, 5 };
    const uint8_t metadata[3] = { 0xde, 0xad, 0xbe };
    const char status_msg[] = "boom";
    nb_envelope_t envelope;
    nb_envelope_t view;
    size_t n;

    memset(&envelope, 0, sizeof(envelope));
    envelope.kind = KIND_REQUEST;
    envelope.encoding = 3;
    envelope.status = -7;
    envelope.request_id = 42;
    envelope.stream_flags = STREAM_FLAG_STREAM | STREAM_FLAG_FIRST;
    envelope.stream_id = 99;
    envelope.stream_offset = 0x1122334455667788ull;
    envelope.stream_total_size = 0x0a0b0c0d0e0f1011ull;
    envelope.timeout_ms = 1234;
    envelope.route = route;
    envelope.metadata = metadata;
    envelope.metadata_len = sizeof(metadata);
    envelope.status_msg = status_msg;
    envelope.status_msg_len = (uint16_t)strlen(status_msg);
    envelope.payload = payload;
    envelope.payload_len = sizeof(payload);
    n = nb_envelope_encode(buf, sizeof(buf), &envelope);
    CHECK(n == nb_envelope_size(&envelope));
    CHECK(nb_envelope_decode(buf, n, &view) == NB_OK);
    CHECK(view.kind == KIND_REQUEST);
    CHECK(view.encoding == 3);
    CHECK(view.status == -7);
    CHECK(view.request_id == 42);
    CHECK(view.stream_flags == (STREAM_FLAG_STREAM | STREAM_FLAG_FIRST));
    CHECK(view.stream_id == 99);
    CHECK(view.stream_offset == 0x1122334455667788ull);
    CHECK(view.stream_total_size == 0x0a0b0c0d0e0f1011ull);
    CHECK(view.route_len == strlen(route));
    CHECK(memcmp(view.route, route, strlen(route)) == 0);
    CHECK(view.metadata_len == sizeof(metadata));
    CHECK(memcmp(view.metadata, metadata, sizeof(metadata)) == 0);
    CHECK(view.status_msg_len == strlen(status_msg));
    CHECK(memcmp(view.status_msg, status_msg, strlen(status_msg)) == 0);
    CHECK(view.timeout_ms == 1234);
    CHECK(view.payload_len == sizeof(payload));
    CHECK(memcmp(view.payload, payload, sizeof(payload)) == 0);
}

static void test_envelope_control_kinds(void)
{
    static const struct {
        uint8_t kind;
        uint8_t opcode;
    } cases[] = {
        { KIND_CANCEL, NB_CTRL_CANCEL },
        { KIND_GOAWAY, NB_CTRL_GOAWAY },
        { KIND_PING, NB_CTRL_PING },
    };
    uint8_t buf[64];
    nb_envelope_t envelope;
    nb_envelope_t view;
    nb_frame_t frame;
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t n;

        memset(&envelope, 0, sizeof(envelope));
        envelope.kind = cases[i].kind;
        envelope.request_id = 77;
        n = nb_envelope_encode(buf, sizeof(buf), &envelope);
        CHECK(n > 0);
        CHECK(nb_frame_decode(buf, n, &frame) == NB_OK);
        CHECK(frame.header.frame_type == NB_FRAME_CONTROL);
        CHECK(frame.header.kind == cases[i].opcode);
        CHECK(nb_envelope_decode(buf, n, &view) == NB_OK);
        CHECK(view.kind == cases[i].kind);
        CHECK(view.request_id == 77);
    }
}

static void test_frame_ext_roundtrip(void)
{
    uint8_t buf[256];
    uint8_t ext[64];
    const uint8_t payload[3] = { 9, 8, 7 };
    const uint8_t deadline[4] = { 0x00, 0x00, 0x05, 0xdc };
    const char route[] = "echo.ping";
    nb_ext_set_t set;
    nb_ext_t got;
    nb_frame_header_t header;
    nb_frame_t frame;
    size_t ext_len;
    size_t n;

    nb_ext_set_init(&set, ext, sizeof(ext));
    CHECK(nb_ext_set_add(&set, NB_EXT_ROUTE, route, strlen(route)));
    CHECK(nb_ext_set_add(&set, NB_EXT_DEADLINE, deadline, sizeof(deadline)));
    CHECK(nb_ext_set_add(&set, NB_EXT_APP_BASE, "x", 1));
    ext_len = set.len;

    memset(&header, 0, sizeof(header));
    header.frame_type = NB_FRAME_DATA;
    header.kind = NB_KIND_REQUEST;
    header.request_id = 7;
    header.ext_len = (uint32_t)ext_len;
    header.payload_len = sizeof(payload);
    n = nb_frame_encode(buf, sizeof(buf), &header, ext, ext_len, payload, sizeof(payload));
    CHECK(n == nb_frame_size(ext_len, sizeof(payload)));
    CHECK(n == (size_t)NB_FRAME_HDR_LEN + ext_len + sizeof(payload));
    CHECK(nb_frame_decode(buf, n, &frame) == NB_OK);
    CHECK(frame.header.frame_type == NB_FRAME_DATA);
    CHECK(frame.header.request_id == 7);
    CHECK(frame.header.payload_len == sizeof(payload));
    CHECK(memcmp(frame.payload, payload, sizeof(payload)) == 0);

    CHECK(nb_ext_get(frame.ext, frame.header.ext_len, NB_EXT_ROUTE, &got) == 1);
    CHECK(got.len == strlen(route));
    CHECK(memcmp(got.value, route, got.len) == 0);
    CHECK(nb_ext_get(frame.ext, frame.header.ext_len, NB_EXT_DEADLINE, &got) == 1);
    CHECK(got.len == sizeof(deadline));
    CHECK(nb_ext_get(frame.ext, frame.header.ext_len, NB_EXT_APP_BASE, &got) == 1);
    CHECK(nb_ext_get(frame.ext, frame.header.ext_len, 0x0999, &got) == 0);

    /* 篡改扩展长度必须被拒绝（越界或 CRC）。 */
    buf[NB_FRAME_HDR_LEN + 2] = 0x10;
    CHECK(nb_frame_decode(buf, n, &frame) == NB_ERR_INVALID);
}

static void test_envelope_rejects_bad_header(void)
{
    uint8_t buf[128];
    uint8_t bad[128];
    const char *route = "echo.ping";
    const uint8_t payload[4] = { 1, 2, 3, 4 };
    nb_envelope_t envelope;
    nb_envelope_t view;
    size_t n;

    memset(&envelope, 0, sizeof(envelope));
    envelope.kind = KIND_REQUEST;
    envelope.route = route;
    envelope.payload = payload;
    envelope.payload_len = sizeof(payload);
    n = nb_envelope_encode(buf, sizeof(buf), &envelope);
    CHECK(n > 0);
    CHECK(nb_envelope_decode(buf, n, &view) == NB_OK);

    memcpy(bad, buf, n);
    bad[0] ^= 0xff;
    CHECK(nb_envelope_decode(bad, n, &view) == NB_ERR_INVALID);

    memcpy(bad, buf, n);
    bad[2] = (uint8_t)(ENVELOPE_VERSION + 1u);
    CHECK(nb_envelope_decode(bad, n, &view) == NB_ERR_INVALID);

    /* 控制段（route 首字节）被篡改必须被 CRC 检出。 */
    memcpy(bad, buf, n);
    bad[ENVELOPE_HDR_LEN] ^= 0x01;
    CHECK(nb_envelope_decode(bad, n, &view) == NB_ERR_INVALID);

    memcpy(bad, buf, n);
    bad[ENVELOPE_OFF_CHECKSUM] ^= 0x01;
    CHECK(nb_envelope_decode(bad, n, &view) == NB_ERR_INVALID);
}

static void test_udp_fragment_roundtrip(void)
{
    uint8_t buf[256];
    const uint8_t chunk[5] = { 1, 2, 3, 4, 5 };
    nb_frag_t desc;
    nb_frag_t view;
    size_t n;

    memset(&desc, 0, sizeof(desc));
    desc.marker = 1;
    desc.pt = PT_DATA;
    desc.seq = 0x1234;
    desc.ts = 0xdeadbeefu;
    desc.ssrc = 0x0a0b0c0du;
    desc.msg_id = 42;
    desc.total_len = sizeof(chunk);
    desc.frag_len = sizeof(chunk);
    desc.flags = FLAG_FIRST | FLAG_LAST;
    desc.chunk = chunk;
    n = nb_frame_build_data(buf, sizeof(buf), &desc);
    CHECK(n > 0);
    CHECK(nb_frame_parse_data(buf, n, &view) == 1);
    CHECK(view.marker == 1);
    CHECK(view.seq == 0x1234);
    CHECK(view.ssrc == 0x0a0b0c0du);
    CHECK(view.msg_id == 42);
    CHECK(view.total_len == sizeof(chunk));
    CHECK(view.frag_len == sizeof(chunk));
    CHECK(view.flags == (FLAG_FIRST | FLAG_LAST));
    CHECK(memcmp(view.chunk, chunk, sizeof(chunk)) == 0);
}

static void test_tcp_opaque_frame(void)
{
    uint8_t buf[64];
    const uint8_t body[4] = { 9, 8, 7, 6 };
    const uint8_t *parsed = NULL;
    size_t parsed_len = 0;
    size_t n;

    n = nb_tcp_build(buf, sizeof(buf), body, sizeof(body));
    CHECK(n == sizeof(body) + TCP_HDR_LEN);
    CHECK(nb_tcp_parse(buf, n, &parsed, &parsed_len) == 1);
    CHECK(parsed_len == sizeof(body));
    CHECK(memcmp(parsed, body, sizeof(body)) == 0);
}

static void test_nack_roundtrip(void)
{
    uint8_t buf[128];
    uint16_t seqs[4] = { 100, 101, 103, 200 };
    uint16_t out[64];
    size_t n;
    int c;

    n = nb_frame_build_nack(buf, sizeof(buf), 0x11111111u, 0x22222222u, seqs, 4);
    CHECK(n > 0);
    c = nb_frame_parse_nack(buf, n, NULL, NULL, out, 64);
    CHECK(c == 4);
    CHECK(out[0] == 100);
    CHECK(out[1] == 101);
    CHECK(out[2] == 103);
    CHECK(out[3] == 200);
}

static void test_parse_rejects_rtcp(void)
{
    uint8_t rtcp[12];
    nb_frag_t view;
    memset(rtcp, 0, sizeof(rtcp));
    rtcp[0] = (uint8_t)(2u << 6);
    rtcp[1] = RTCP_RTPFB;
    CHECK(nb_frame_parse_data(rtcp, sizeof(rtcp), &view) == 0);
}

static void test_rtcp_sr_roundtrip(void)
{
    uint8_t buf[256];
    nb_rtcp_report_block_t rb;
    nb_rtcp_sr_t sr;
    size_t n;

    memset(&rb, 0, sizeof(rb));
    rb.ssrc = 0x01020304u;
    rb.fraction_lost = 7;
    rb.cumulative_lost = -3;
    rb.ext_highest_seq = 0x0a0b0c0du;
    rb.jitter = 1234;
    rb.lsr = 0x11223344u;
    rb.dlsr = 0x55667788u;

    n = nb_frame_build_sr(
        buf, sizeof(buf), 0xdeadbeefu, 0x11111111u, 0x22222222u, 0x33333333u, 100, 2000, &rb, 1);
    CHECK(n == 52);
    memset(&sr, 0, sizeof(sr));
    CHECK(nb_frame_parse_sr(buf, n, &sr) == 1);
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

static void test_rtcp_rr_roundtrip(void)
{
    uint8_t buf[128];
    nb_rtcp_report_block_t rb;
    nb_rtcp_rr_t rr;
    size_t n;

    memset(&rb, 0, sizeof(rb));
    rb.ssrc = 0x01010101u;
    rb.cumulative_lost = 5;
    n = nb_frame_build_rr(buf, sizeof(buf), 0x0f0f0f0fu, &rb, 1);
    CHECK(n == 32);
    memset(&rr, 0, sizeof(rr));
    CHECK(nb_frame_parse_rr(buf, n, &rr) == 1);
    CHECK(rr.ssrc == 0x0f0f0f0fu);
    CHECK(rr.rb_count == 1);
    CHECK(rr.rb[0].ssrc == 0x01010101u);
    CHECK(rr.rb[0].cumulative_lost == 5);
}

static void test_rtcp_sdes_and_dispatch(void)
{
    uint8_t buf[128];
    nb_frag_t view;
    size_t n;

    n = nb_frame_build_sdes_cname(buf, sizeof(buf), 0xabcdef01u, "rpc");
    CHECK(n == 16);                                  /* 4 头 + 4 ssrc + 2 项头 + 4 名 + 2 填充 */
    CHECK(nb_frame_parse_data(buf, n, &view) == 0); /* SDES 不是数据包 */

    memset(buf, 0, sizeof(buf));
    buf[0] = (uint8_t)(2u << 6);
    buf[1] = RTCP_SR;
    CHECK(nb_frame_parse_sr(buf, 3, NULL) == 0);
}

static void test_rtcp_bye_roundtrip(void)
{
    uint8_t buf[16];
    uint32_t ssrc = 0;
    size_t n;

    n = nb_frame_build_bye(buf, sizeof(buf), 0x0badf00du);
    CHECK(n == 8);
    CHECK(nb_frame_parse_bye(buf, n, &ssrc) == 1);
    CHECK(ssrc == 0x0badf00du);
    CHECK(nb_frame_parse_bye(buf, 4, NULL) == 0);
}

int main(void)
{
    test_envelope_roundtrip();
    test_envelope_control_kinds();
    test_frame_ext_roundtrip();
    test_envelope_rejects_bad_header();
    test_udp_fragment_roundtrip();
    test_tcp_opaque_frame();
    test_nack_roundtrip();
    test_parse_rejects_rtcp();
    test_rtcp_sr_roundtrip();
    test_rtcp_rr_roundtrip();
    test_rtcp_sdes_and_dispatch();
    test_rtcp_bye_roundtrip();
    if (g_fail) {
        nb_log_error("nb_framing: FAIL");
        return 1;
    }
    nb_log_info("nb_framing: NB_OK");
    return 0;
}
