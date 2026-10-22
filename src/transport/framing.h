#ifndef FRAMING_H
#define FRAMING_H

/* Transport framing only. RPC envelope bytes are opaque here. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UDP/RTP */
#define RTP_HDR_LEN 12
#define FRAG_HDR_LEN 20
#define PT_DATA 96
#define PT_RTX 97
#define PT_FEC 98
#define RTCP_RTPFB 205
#define RTCP_FMT_NACK 1

/* RTCP packet types (RFC 3550) */
#define RTCP_SR 200
#define RTCP_RR 201
#define RTCP_SDES 202
#define RTCP_BYE 203
#define RTCP_SDES_CNAME 1

/* 单个接收报告块（RFC 3550 §6.4.1）。 */
typedef struct nb_rtcp_report_block {
    uint32_t ssrc;
    uint8_t fraction_lost;
    int32_t cumulative_lost; /* 24 位有符号，已做符号扩展 */
    uint32_t ext_highest_seq;
    uint32_t jitter;
    uint32_t lsr;
    uint32_t dlsr;
} nb_rtcp_report_block_t;

/* Sender Report。 */
typedef struct nb_rtcp_sr {
    uint32_t ssrc;
    uint32_t ntp_msw;
    uint32_t ntp_lsw;
    uint32_t rtp_ts;
    uint32_t packet_count;
    uint32_t octet_count;
    int rb_count;
    nb_rtcp_report_block_t rb[31];
} nb_rtcp_sr_t;

/* Receiver Report。 */
typedef struct nb_rtcp_rr {
    uint32_t ssrc;
    int rb_count;
    nb_rtcp_report_block_t rb[31];
} nb_rtcp_rr_t;

#define FLAG_FIRST 0x01
#define FLAG_LAST 0x02

/* 单个分片描述：build 读取字段，parse 回填字段（无需分开的 desc/view）。 */
typedef struct nb_frag {
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
} nb_frag_t;

size_t nb_frame_build_data(uint8_t *buf, size_t cap, const nb_frag_t *desc);
/*
 * 只写 RTP + 分片头（不拷贝 payload）。desc->frag_len 仍写入头部，但
 * desc->chunk 被忽略，调用方随后自行拷贝分片数据到 buf + 返回值处。
 * 返回写入的头部字节数，0 表示失败。
 */
size_t nb_frame_build_frag_header(uint8_t *buf, size_t cap, const nb_frag_t *desc);
int nb_frame_parse_data(const uint8_t *buf, size_t len, nb_frag_t *view);

size_t nb_frame_build_nack(uint8_t *buf,
                            size_t cap,
                            uint32_t sender_ssrc,
                            uint32_t media_ssrc,
                            const uint16_t *seqs,
                            size_t count);
int nb_frame_parse_nack(const uint8_t *buf,
                         size_t len,
                         uint32_t *sender_ssrc,
                         uint32_t *media_ssrc,
                         uint16_t *out_seqs,
                         size_t out_cap);

/* RTCP 报告编解码：build 返回写入字节数（0 失败）；parse 返回 1 成功、0 非本类型、-1 非法。 */
size_t nb_frame_build_sr(uint8_t *buf,
                          size_t cap,
                          uint32_t ssrc,
                          uint32_t ntp_msw,
                          uint32_t ntp_lsw,
                          uint32_t rtp_ts,
                          uint32_t packet_count,
                          uint32_t octet_count,
                          const nb_rtcp_report_block_t *rb,
                          int rb_count);
size_t nb_frame_build_rr(
    uint8_t *buf, size_t cap, uint32_t ssrc, const nb_rtcp_report_block_t *rb, int rb_count);
size_t nb_frame_build_sdes_cname(uint8_t *buf, size_t cap, uint32_t ssrc, const char *cname);
size_t nb_frame_build_bye(uint8_t *buf, size_t cap, uint32_t ssrc);
int nb_frame_parse_sr(const uint8_t *buf, size_t len, nb_rtcp_sr_t *out);
int nb_frame_parse_rr(const uint8_t *buf, size_t len, nb_rtcp_rr_t *out);
int nb_frame_parse_bye(const uint8_t *buf, size_t len, uint32_t *ssrc);

/* TCP: big-endian u32 body length + opaque envelope bytes. */
#define TCP_HDR_LEN 4
size_t nb_tcp_frame_size(const uint8_t *buf, size_t len);
size_t nb_tcp_build(uint8_t *buf, size_t cap, const uint8_t *body, size_t body_len);
int nb_tcp_parse(const uint8_t *buf, size_t len, const uint8_t **body, size_t *body_len);

#ifdef __cplusplus
}
#endif

#endif /* FRAMING_H */
