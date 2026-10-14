#include "zrpc_framing.h"

#include "zrpc_util.h"

#include <string.h>

static int seq_diff16(uint16_t seq, uint16_t base) {
    return (int)(int16_t)(uint16_t)(seq - base);
}

size_t zrpc_frame_build_frag_header(uint8_t *buf, size_t cap, const zrpc_frag_desc_t *d) {
    uint8_t *p;
    if (!buf || !d || d->frag_off > d->total_len || d->frag_len > d->total_len - d->frag_off) {
        return 0;
    }
    if (cap < (size_t)ZRPC_RTP_HDR_LEN + ZRPC_FRAG_HDR_LEN) {
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
    return (size_t)ZRPC_RTP_HDR_LEN + ZRPC_FRAG_HDR_LEN;
}

size_t zrpc_frame_build_data(uint8_t *buf, size_t cap, const zrpc_frag_desc_t *d) {
    size_t hdr;
    size_t need;

    if (!buf || !d || (d->frag_len > 0 && !d->chunk)) {
        return 0;
    }
    need = (size_t)ZRPC_RTP_HDR_LEN + ZRPC_FRAG_HDR_LEN + d->frag_len;
    if (need > cap) {
        return 0;
    }
    hdr = zrpc_frame_build_frag_header(buf, cap, d);
    if (!hdr) {
        return 0;
    }
    if (d->frag_len > 0) {
        memcpy(buf + hdr, d->chunk, d->frag_len);
    }
    return need;
}

int zrpc_frame_parse_data(const uint8_t *buf, size_t len, zrpc_frag_view_t *v) {
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
    if (pt != ZRPC_PT_DATA && pt != ZRPC_PT_RTX && pt != ZRPC_PT_FEC) {
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

size_t zrpc_frame_build_nack(uint8_t *buf, size_t cap, uint32_t sender_ssrc, uint32_t media_ssrc,
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

int zrpc_frame_parse_nack(const uint8_t *buf, size_t len, uint32_t *sender_ssrc, uint32_t *media_ssrc,
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

/* ---- RTCP SR/RR/SDES ---- */

static void put_report_block(uint8_t *p, const zrpc_rtcp_report_block_t *rb) {
    zrpc_put_u32(p, rb->ssrc);
    p[4] = rb->fraction_lost;
    p[5] = (uint8_t)((uint32_t)rb->cumulative_lost >> 16);
    p[6] = (uint8_t)((uint32_t)rb->cumulative_lost >> 8);
    p[7] = (uint8_t)(uint32_t)rb->cumulative_lost;
    zrpc_put_u32(p + 8, rb->ext_highest_seq);
    zrpc_put_u32(p + 12, rb->jitter);
    zrpc_put_u32(p + 16, rb->lsr);
    zrpc_put_u32(p + 20, rb->dlsr);
}

static void get_report_block(const uint8_t *p, zrpc_rtcp_report_block_t *rb) {
    uint32_t raw;
    rb->ssrc = zrpc_get_u32(p);
    rb->fraction_lost = p[4];
    raw = zrpc_get_u32(p + 4) & 0x00ffffffu;
    if (raw & 0x00800000u) {
        raw |= 0xff000000u; /* 24 位有符号扩展 */
    }
    rb->cumulative_lost = (int32_t)raw;
    rb->ext_highest_seq = zrpc_get_u32(p + 8);
    rb->jitter = zrpc_get_u32(p + 12);
    rb->lsr = zrpc_get_u32(p + 16);
    rb->dlsr = zrpc_get_u32(p + 20);
}

size_t zrpc_frame_build_sr(uint8_t *buf, size_t cap, uint32_t ssrc, uint32_t ntp_msw,
                           uint32_t ntp_lsw, uint32_t rtp_ts, uint32_t packet_count,
                           uint32_t octet_count, const zrpc_rtcp_report_block_t *rb, int rb_count) {
    size_t need;
    int i;
    if (!buf || rb_count < 0 || rb_count > 31 || (rb_count > 0 && !rb)) {
        return 0;
    }
    need = 28u + (size_t)rb_count * 24u;
    if (cap < need) {
        return 0;
    }
    buf[0] = (uint8_t)((2u << 6) | (uint8_t)(rb_count & 0x1f));
    buf[1] = ZRPC_RTCP_SR;
    zrpc_put_u16(buf + 2, (uint16_t)(need / 4u - 1u));
    zrpc_put_u32(buf + 4, ssrc);
    zrpc_put_u32(buf + 8, ntp_msw);
    zrpc_put_u32(buf + 12, ntp_lsw);
    zrpc_put_u32(buf + 16, rtp_ts);
    zrpc_put_u32(buf + 20, packet_count);
    zrpc_put_u32(buf + 24, octet_count);
    for (i = 0; i < rb_count; i++) {
        put_report_block(buf + 28 + (size_t)i * 24u, &rb[i]);
    }
    return need;
}

size_t zrpc_frame_build_rr(uint8_t *buf, size_t cap, uint32_t ssrc,
                           const zrpc_rtcp_report_block_t *rb, int rb_count) {
    size_t need;
    int i;
    if (!buf || rb_count < 0 || rb_count > 31 || (rb_count > 0 && !rb)) {
        return 0;
    }
    need = 8u + (size_t)rb_count * 24u;
    if (cap < need) {
        return 0;
    }
    buf[0] = (uint8_t)((2u << 6) | (uint8_t)(rb_count & 0x1f));
    buf[1] = ZRPC_RTCP_RR;
    zrpc_put_u16(buf + 2, (uint16_t)(need / 4u - 1u));
    zrpc_put_u32(buf + 4, ssrc);
    for (i = 0; i < rb_count; i++) {
        put_report_block(buf + 8 + (size_t)i * 24u, &rb[i]);
    }
    return need;
}

size_t zrpc_frame_build_sdes_cname(uint8_t *buf, size_t cap, uint32_t ssrc, const char *cname) {
    size_t clen;
    size_t chunk;
    size_t pad;
    size_t need;
    size_t i;
    uint8_t *p;
    if (!buf) {
        return 0;
    }
    if (!cname) {
        cname = "";
    }
    clen = strlen(cname);
    if (clen > 255) {
        clen = 255;
    }
    chunk = 4u + 2u + clen;
    pad = (4u - (chunk & 3u)) & 3u;
    chunk += pad;
    need = 4u + chunk;
    if (cap < need) {
        return 0;
    }
    buf[0] = (uint8_t)((2u << 6) | 1u); /* SC = 1 个 SSRC 块 */
    buf[1] = ZRPC_RTCP_SDES;
    zrpc_put_u16(buf + 2, (uint16_t)(need / 4u - 1u));
    p = buf + 4;
    zrpc_put_u32(p, ssrc);
    p[4] = ZRPC_RTCP_SDES_CNAME;
    p[5] = (uint8_t)clen;
    memcpy(p + 6, cname, clen);
    for (i = 0; i < pad; i++) {
        p[6 + clen + i] = 0;
    }
    return need;
}

size_t zrpc_frame_build_bye(uint8_t *buf, size_t cap, uint32_t ssrc) {
    if (!buf || cap < 8) {
        return 0;
    }
    buf[0] = (uint8_t)((2u << 6) | 1u); /* SC = 1 个 SSRC */
    buf[1] = ZRPC_RTCP_BYE;
    zrpc_put_u16(buf + 2, 1); /* 2 个 32 位字 - 1 */
    zrpc_put_u32(buf + 4, ssrc);
    return 8;
}

int zrpc_frame_parse_sr(const uint8_t *buf, size_t len, zrpc_rtcp_sr_t *out) {
    int cnt;
    int i;
    size_t need;
    if (!buf || !out || len < 4) {
        return 0;
    }
    if ((buf[0] >> 6) != 2 || buf[1] != ZRPC_RTCP_SR) {
        return 0;
    }
    if (len < 28) {
        return -1;
    }
    cnt = buf[0] & 0x1f;
    if (cnt > 31) {
        cnt = 31;
    }
    need = 28u + (size_t)cnt * 24u;
    if (len < need) {
        return -1;
    }
    out->ssrc = zrpc_get_u32(buf + 4);
    out->ntp_msw = zrpc_get_u32(buf + 8);
    out->ntp_lsw = zrpc_get_u32(buf + 12);
    out->rtp_ts = zrpc_get_u32(buf + 16);
    out->packet_count = zrpc_get_u32(buf + 20);
    out->octet_count = zrpc_get_u32(buf + 24);
    out->rb_count = cnt;
    for (i = 0; i < cnt; i++) {
        get_report_block(buf + 28 + (size_t)i * 24u, &out->rb[i]);
    }
    return 1;
}

int zrpc_frame_parse_rr(const uint8_t *buf, size_t len, zrpc_rtcp_rr_t *out) {
    int cnt;
    int i;
    size_t need;
    if (!buf || !out || len < 4) {
        return 0;
    }
    if ((buf[0] >> 6) != 2 || buf[1] != ZRPC_RTCP_RR) {
        return 0;
    }
    if (len < 8) {
        return -1;
    }
    cnt = buf[0] & 0x1f;
    if (cnt > 31) {
        cnt = 31;
    }
    need = 8u + (size_t)cnt * 24u;
    if (len < need) {
        return -1;
    }
    out->ssrc = zrpc_get_u32(buf + 4);
    out->rb_count = cnt;
    for (i = 0; i < cnt; i++) {
        get_report_block(buf + 8 + (size_t)i * 24u, &out->rb[i]);
    }
    return 1;
}

int zrpc_frame_parse_bye(const uint8_t *buf, size_t len, uint32_t *ssrc) {
    if (!buf || len < 8) {
        return 0;
    }
    if ((buf[0] >> 6) != 2 || buf[1] != ZRPC_RTCP_BYE) {
        return 0;
    }
    if (ssrc) {
        *ssrc = zrpc_get_u32(buf + 4);
    }
    return 1;
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
