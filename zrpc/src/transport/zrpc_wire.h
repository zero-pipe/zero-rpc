#ifndef ZRPC_WIRE_H
#define ZRPC_WIRE_H

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

size_t zrpc_wire_build_data(uint8_t *buf, size_t cap, const zrpc_frag_desc_t *desc);
int zrpc_wire_parse_data(const uint8_t *buf, size_t len, zrpc_frag_view_t *view);

size_t zrpc_wire_build_nack(uint8_t *buf, size_t cap, uint32_t sender_ssrc, uint32_t media_ssrc,
                            const uint16_t *seqs, size_t count);
int zrpc_wire_parse_nack(const uint8_t *buf, size_t len, uint32_t *sender_ssrc, uint32_t *media_ssrc,
                         uint16_t *out_seqs, size_t out_cap);

/* TCP: big-endian u32 body length + opaque envelope bytes. */
#define ZRPC_TCP_HDR_LEN 4
size_t zrpc_tcp_frame_size(const uint8_t *buf, size_t len);
size_t zrpc_tcp_build(uint8_t *buf, size_t cap, const uint8_t *body, size_t body_len);
int zrpc_tcp_parse(const uint8_t *buf, size_t len, const uint8_t **body, size_t *body_len);

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_WIRE_H */
