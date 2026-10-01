#include "zrpc_wire.h"

#include "zrpc_util.h"

#include <string.h>

static int seq_diff16(uint16_t seq, uint16_t base) {
    return (int)(int16_t)(uint16_t)(seq - base);
}

size_t zrpc_wire_build_data(uint8_t *buf, size_t cap, const zrpc_frag_desc_t *d) {
    size_t need;
    uint8_t *p;
    if (!buf || !d || d->frag_len > 0 && !d->chunk) {
        return 0;
    }
    need = (size_t)ZRPC_RTP_HDR_LEN + ZRPC_FRAG_HDR_LEN + d->frag_len;
    if (need > cap || d->frag_off > d->total_len || d->frag_len > d->total_len - d->frag_off) {
        return 0;
    }
    p = buf;
    p[0] = 0x80;
    p[1] = (uint8_t)((d->marker ? 0x80 : 0x00) | (d->pt & 0x7f));
    zrpc_put_u16(p + 2, d->seq);
    zrpc_put_u32(p + 4, d->ts);
    zrpc_put_u32(p + 8, d->ssrc);
    p += ZRPC_RTP_HDR_LEN;
    zrpc_put_u32(p + 0, d->msg_id);
    zrpc_put_u32(p + 4, d->total_len);
    zrpc_put_u32(p + 8, d->frag_off);
    zrpc_put_u32(p + 12, d->frag_len);
    p[16] = d->flags;
    p[17] = 0;
    zrpc_put_u16(p + 18, 0);
    p += ZRPC_FRAG_HDR_LEN;
    if (d->frag_len > 0) {
        memcpy(p, d->chunk, d->frag_len);
    }
    return need;
}

int zrpc_wire_parse_data(const uint8_t *buf, size_t len, zrpc_frag_view_t *v) {
    const uint8_t *p;
    uint8_t pt;
    size_t body;
    if (!buf || !v || len < ZRPC_RTP_HDR_LEN) {
        return -1;
    }
    if ((buf[0] >> 6) != 2) {
        return -1;
    }
    pt = (uint8_t)(buf[1] & 0x7f);
    if (pt != ZRPC_PT_DATA && pt != ZRPC_PT_RTX) {
        return 0; /* RTCP or other non-data packet */
    }
    if (len < (size_t)ZRPC_RTP_HDR_LEN + ZRPC_FRAG_HDR_LEN) {
        return -1;
    }
    v->marker = (buf[1] & 0x80) ? 1 : 0;
    v->pt = pt;
    v->seq = zrpc_get_u16(buf + 2);
    v->ts = zrpc_get_u32(buf + 4);
    v->ssrc = zrpc_get_u32(buf + 8);
    p = buf + ZRPC_RTP_HDR_LEN;
    v->msg_id = zrpc_get_u32(p + 0);
    v->total_len = zrpc_get_u32(p + 4);
    v->frag_off = zrpc_get_u32(p + 8);
    v->frag_len = zrpc_get_u32(p + 12);
    v->flags = p[16];
    body = len - ZRPC_RTP_HDR_LEN - ZRPC_FRAG_HDR_LEN;
    if (body != v->frag_len || v->frag_off > v->total_len ||
        v->frag_len > v->total_len - v->frag_off) {
        return -1;
    }
    v->chunk = p + ZRPC_FRAG_HDR_LEN;
    return 1;
}

size_t zrpc_wire_build_nack(uint8_t *buf, size_t cap, uint32_t sender_ssrc, uint32_t media_ssrc,
                            const uint16_t *seqs, size_t count) {
    size_t i = 0;
    uint8_t *p;
    size_t total;
    if (!buf || (count > 0 && !seqs) || count == 0 || cap < 12) {
        return 0;
    }
    buf[0] = (uint8_t)((2u << 6) | ZRPC_RTCP_FMT_NACK);
    buf[1] = ZRPC_RTCP_RTPFB;
    zrpc_put_u32(buf + 4, sender_ssrc);
    zrpc_put_u32(buf + 8, media_ssrc);
    p = buf + 12;
    while (i < count) {
        uint16_t pid = seqs[i++];
        uint16_t blp = 0;
        while (i < count) {
            int d = seq_diff16(seqs[i], pid);
            if (d < 1 || d > 16) {
                break;
            }
            blp |= (uint16_t)(1u << (d - 1));
            i++;
        }
        if ((size_t)(p - buf) + 4 > cap) {
            return 0;
        }
        zrpc_put_u16(p, pid);
        zrpc_put_u16(p + 2, blp);
        p += 4;
    }
    total = (size_t)(p - buf);
    zrpc_put_u16(buf + 2, (uint16_t)((total / 4) - 1));
    return total;
}

int zrpc_wire_parse_nack(const uint8_t *buf, size_t len, uint32_t *sender_ssrc, uint32_t *media_ssrc,
                         uint16_t *out_seqs, size_t out_cap) {
    size_t off;
    size_t n = 0;
    if (!buf || len < 12 || (buf[0] >> 6) != 2 || buf[1] != ZRPC_RTCP_RTPFB ||
        (buf[0] & 0x1f) != ZRPC_RTCP_FMT_NACK) {
        return -1;
    }
    if (sender_ssrc) {
        *sender_ssrc = zrpc_get_u32(buf + 4);
    }
    if (media_ssrc) {
        *media_ssrc = zrpc_get_u32(buf + 8);
    }
    for (off = 12; off + 4 <= len; off += 4) {
        uint16_t pid = zrpc_get_u16(buf + off);
        uint16_t blp = zrpc_get_u16(buf + off + 2);
        int b;
        if (out_seqs && n < out_cap) {
            out_seqs[n++] = pid;
        }
        for (b = 0; b < 16; b++) {
            if ((blp & (1u << b)) && out_seqs && n < out_cap) {
                out_seqs[n++] = (uint16_t)(pid + 1 + b);
            }
        }
    }
    return (int)n;
}

size_t zrpc_tcp_frame_size(const uint8_t *buf, size_t len) {
    uint32_t body_len;
    if (!buf || len < ZRPC_TCP_HDR_LEN) {
        return 0;
    }
    body_len = zrpc_get_u32(buf);
    if (body_len == 0) {
        return 0;
    }
    return (size_t)body_len + ZRPC_TCP_HDR_LEN;
}

size_t zrpc_tcp_build(uint8_t *buf, size_t cap, const uint8_t *body, size_t body_len) {
    if (!buf || !body || body_len == 0 || body_len > 0xffffffffu ||
        body_len + ZRPC_TCP_HDR_LEN > cap) {
        return 0;
    }
    zrpc_put_u32(buf, (uint32_t)body_len);
    memcpy(buf + ZRPC_TCP_HDR_LEN, body, body_len);
    return body_len + ZRPC_TCP_HDR_LEN;
}

int zrpc_tcp_parse(const uint8_t *buf, size_t len, const uint8_t **body, size_t *body_len) {
    size_t frame;
    if (!buf || !body || !body_len || len < ZRPC_TCP_HDR_LEN) {
        return -1;
    }
    frame = zrpc_tcp_frame_size(buf, len);
    if (frame == 0 || frame > len) {
        return -1;
    }
    *body_len = frame - ZRPC_TCP_HDR_LEN;
    *body = buf + ZRPC_TCP_HDR_LEN;
    return 1;
}
