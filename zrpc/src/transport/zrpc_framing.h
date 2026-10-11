#ifndef ZRPC_FRAMING_H
#define ZRPC_FRAMING_H

/* Transport framing only. RPC envelope bytes are opaque here. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UDP/RTP */
#define ZRPC_RTP_HDR_LEN 12
#define ZRPC_FRAG_HDR_LEN 20
#define ZRPC_PT_DATA 96
#define ZRPC_PT_RTX 97
#define ZRPC_RTCP_RTPFB 205
#define ZRPC_RTCP_FMT_NACK 1

/* RTCP packet types (RFC 3550) */
#define ZRPC_RTCP_SR 200
#define ZRPC_RTCP_RR 201
#define ZRPC_RTCP_SDES 202
#define ZRPC_RTCP_SDES_CNAME 1

/* 单个接收报告块（RFC 3550 §6.4.1）。 */
typedef struct zrpc_rtcp_report_block {
    uint32_t ssrc;
    uint8_t fraction_lost;
    int32_t cumulative_lost; /* 24 位有符号，已做符号扩展 */
    uint32_t ext_highest_seq;
    uint32_t jitter;
    uint32_t lsr;
    uint32_t dlsr;
} zrpc_rtcp_report_block_t;

/* Sender Report。 */
typedef struct zrpc_rtcp_sr {
    uint32_t ssrc;
    uint32_t ntp_msw;
    uint32_t ntp_lsw;
    uint32_t rtp_ts;
    uint32_t packet_count;
    uint32_t octet_count;
    int rb_count;
    zrpc_rtcp_report_block_t rb[31];
} zrpc_rtcp_sr_t;

/* Receiver Report。 */
typedef struct zrpc_rtcp_rr {
    uint32_t ssrc;
    int rb_count;
    zrpc_rtcp_report_block_t rb[31];
} zrpc_rtcp_rr_t;

#define ZRPC_FLAG_FIRST 0x01
#define ZRPC_FLAG_LAST 0x02

typedef struct zrpc_frag_desc {
    int marker;
    uint8_t pt;
    uint16_t seq;
    uint32_t ts;
    uint32_t ssrc;
    uint32_t msg_id;
    uint32_t total_len;
    uint32_t frag_off;
    uint32_t frag_len;
    uint8_t flags;
    const uint8_t *chunk;
} zrpc_frag_desc_t;

typedef struct zrpc_frag_view {
    int marker;
    uint8_t pt;
    uint16_t seq;
    uint32_t ts;
    uint32_t ssrc;
    uint32_t msg_id;
    uint32_t total_len;
    uint32_t frag_off;
    uint32_t frag_len;
    uint8_t flags;
    const uint8_t *chunk;
} zrpc_frag_view_t;

size_t zrpc_frame_build_data(uint8_t *buf, size_t cap, const zrpc_frag_desc_t *desc);
/*
 * 只写 RTP + 分片头（不拷贝 payload）。desc->frag_len 仍写入头部，但
 * desc->chunk 被忽略，调用方随后自行拷贝分片数据到 buf + 返回值处。
 * 返回写入的头部字节数，0 表示失败。
 */
size_t zrpc_frame_build_frag_header(uint8_t *buf, size_t cap, const zrpc_frag_desc_t *desc);
int zrpc_frame_parse_data(const uint8_t *buf, size_t len, zrpc_frag_view_t *view);

size_t zrpc_frame_build_nack(uint8_t *buf, size_t cap, uint32_t sender_ssrc, uint32_t media_ssrc,
                            const uint16_t *seqs, size_t count);
int zrpc_frame_parse_nack(const uint8_t *buf, size_t len, uint32_t *sender_ssrc, uint32_t *media_ssrc,
                         uint16_t *out_seqs, size_t out_cap);

/* RTCP 报告编解码：build 返回写入字节数（0 失败）；parse 返回 1 成功、0 非本类型、-1 非法。 */
size_t zrpc_frame_build_sr(uint8_t *buf, size_t cap, uint32_t ssrc, uint32_t ntp_msw,
                           uint32_t ntp_lsw, uint32_t rtp_ts, uint32_t packet_count,
                           uint32_t octet_count, const zrpc_rtcp_report_block_t *rb, int rb_count);
size_t zrpc_frame_build_rr(uint8_t *buf, size_t cap, uint32_t ssrc,
                           const zrpc_rtcp_report_block_t *rb, int rb_count);
size_t zrpc_frame_build_sdes_cname(uint8_t *buf, size_t cap, uint32_t ssrc, const char *cname);
int zrpc_frame_parse_sr(const uint8_t *buf, size_t len, zrpc_rtcp_sr_t *out);
int zrpc_frame_parse_rr(const uint8_t *buf, size_t len, zrpc_rtcp_rr_t *out);

/* TCP: big-endian u32 body length + opaque envelope bytes. */
#define ZRPC_TCP_HDR_LEN 4
size_t zrpc_tcp_frame_size(const uint8_t *buf, size_t len);
size_t zrpc_tcp_build(uint8_t *buf, size_t cap, const uint8_t *body, size_t body_len);
int zrpc_tcp_parse(const uint8_t *buf, size_t len, const uint8_t **body, size_t *body_len);

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_FRAMING_H */
