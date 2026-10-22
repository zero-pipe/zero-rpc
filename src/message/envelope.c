#include "envelope.h"

#include "bytes.h"

#include <string.h>

static size_t envelope_route_len(const zri_envelope_t *envelope)
{
    return envelope->route ? strlen(envelope->route) : 0;
}

static int envelope_has_stream(const zri_envelope_t *envelope)
{
    return (envelope->stream_flags & STREAM_FLAG_STREAM) != 0;
}

/* 扩展区字节数（不含固定头与 payload）。 */
static size_t envelope_ext_size(const zri_envelope_t *envelope, size_t route_len)
{
    size_t n = 0;

    if (route_len > 0) {
        n += ZRI_EXT_TLV_HDR_LEN + route_len;
    }
    if (envelope->metadata_len > 0) {
        n += ZRI_EXT_TLV_HDR_LEN + envelope->metadata_len;
    }
    if (envelope->status != 0 || envelope->status_msg_len > 0) {
        n += ZRI_EXT_TLV_HDR_LEN + ZRI_EXT_STATUS_HDR_LEN + envelope->status_msg_len;
    }
    if (envelope_has_stream(envelope)) {
        n += ZRI_EXT_TLV_HDR_LEN + ZRI_EXT_STREAM_LEN;
    }
    if (envelope->timeout_ms > 0) {
        n += ZRI_EXT_TLV_HDR_LEN + ZRI_EXT_DEADLINE_LEN;
    }
    return n;
}

static size_t ext_reserve(uint8_t *buf, size_t cap, size_t off, uint16_t tag, size_t len)
{
    if (len > 0xffffu || off > cap || cap - off < ZRI_EXT_TLV_HDR_LEN + len) {
        return 0;
    }
    zri_put_u16(buf + off, tag);
    zri_put_u16(buf + off + 2, (uint16_t)len);
    return off + ZRI_EXT_TLV_HDR_LEN;
}

static size_t envelope_build_ext(const zri_envelope_t *envelope,
                                 size_t route_len,
                                 uint8_t *buf,
                                 size_t cap)
{
    size_t off = 0;

    if (route_len > 0) {
        off = ext_reserve(buf, cap, off, ZRI_EXT_ROUTE, route_len);
        if (off == 0) {
            return 0;
        }
        memcpy(buf + off, envelope->route, route_len);
        off += route_len;
    }
    if (envelope->metadata_len > 0) {
        off = ext_reserve(buf, cap, off, ZRI_EXT_METADATA, envelope->metadata_len);
        if (off == 0) {
            return 0;
        }
        memcpy(buf + off, envelope->metadata, envelope->metadata_len);
        off += envelope->metadata_len;
    }
    if (envelope->status != 0 || envelope->status_msg_len > 0) {
        off = ext_reserve(
            buf, cap, off, ZRI_EXT_STATUS, ZRI_EXT_STATUS_HDR_LEN + envelope->status_msg_len);
        if (off == 0) {
            return 0;
        }
        zri_put_u32(buf + off, (uint32_t)envelope->status);
        off += ZRI_EXT_STATUS_HDR_LEN;
        if (envelope->status_msg_len > 0) {
            memcpy(buf + off, envelope->status_msg, envelope->status_msg_len);
            off += envelope->status_msg_len;
        }
    }
    if (envelope_has_stream(envelope)) {
        off = ext_reserve(buf, cap, off, ZRI_EXT_STREAM, ZRI_EXT_STREAM_LEN);
        if (off == 0) {
            return 0;
        }
        buf[off] = (uint8_t)(envelope->stream_flags & (STREAM_FIRST | STREAM_LAST | STREAM_CANCEL));
        zri_put_u64(buf + off + 1, envelope->stream_offset);
        zri_put_u64(buf + off + 9, envelope->stream_total_size);
        off += ZRI_EXT_STREAM_LEN;
    }
    if (envelope->timeout_ms > 0) {
        off = ext_reserve(buf, cap, off, ZRI_EXT_DEADLINE, ZRI_EXT_DEADLINE_LEN);
        if (off == 0) {
            return 0;
        }
        zri_put_u32(buf + off, envelope->timeout_ms);
        off += ZRI_EXT_DEADLINE_LEN;
    }
    return off;
}

static void envelope_fill_header(const zri_envelope_t *envelope,
                                 size_t ext_len,
                                 zri_frame_header_t *header)
{
    memset(header, 0, sizeof(*header));
    if (envelope->kind == KIND_CANCEL) {
        header->frame_type = ZRI_FRAME_CONTROL;
        header->kind = ZRI_CTRL_CANCEL;
    } else {
        header->frame_type = ZRI_FRAME_DATA;
        header->kind = envelope->kind;
    }
    header->encoding = envelope->encoding;
    header->flags = envelope->flags;
    header->stream_id = envelope->stream_id;
    header->request_id = envelope->request_id;
    header->ext_len = (uint32_t)ext_len;
    header->payload_len = (uint32_t)envelope->payload_len;
}

size_t zri_envelope_size(const zri_envelope_t *envelope)
{
    size_t route_len;

    if (!envelope) {
        return 0;
    }
    route_len = envelope_route_len(envelope);
    return (size_t)ENVELOPE_HDR_LEN + envelope_ext_size(envelope, route_len) +
           envelope->payload_len;
}

size_t zri_envelope_encode_prefix(uint8_t *buf, size_t cap, const zri_envelope_t *envelope)
{
    zri_frame_header_t header;
    size_t route_len;
    size_t ext_len;
    size_t expect;

    if (!buf || !envelope) {
        return 0;
    }
    if (envelope->payload_len > 0xffffffffu || envelope->metadata_len > 0xffffu ||
        envelope->status_msg_len > 0xffffu) {
        return 0;
    }
    if ((envelope->metadata_len > 0 && !envelope->metadata) ||
        (envelope->status_msg_len > 0 && !envelope->status_msg)) {
        return 0;
    }
    route_len = envelope_route_len(envelope);
    if (route_len > 0xffffu || cap < ENVELOPE_HDR_LEN) {
        return 0;
    }
    expect = envelope_ext_size(envelope, route_len);
    ext_len = envelope_build_ext(
        envelope, route_len, buf + ENVELOPE_HDR_LEN, cap - ENVELOPE_HDR_LEN);
    if (ext_len != expect) {
        return 0;
    }
    envelope_fill_header(envelope, ext_len, &header);
    if (!zri_frame_write_header(buf, ext_len, &header)) {
        return 0;
    }
    return (size_t)ENVELOPE_HDR_LEN + ext_len;
}

size_t zri_envelope_encode(uint8_t *buf, size_t cap, const zri_envelope_t *envelope)
{
    size_t need;
    size_t prefix_len;

    if (!buf || !envelope) {
        return 0;
    }
    if (envelope->payload_len > 0 && !envelope->payload) {
        return 0;
    }
    need = zri_envelope_size(envelope);
    if (need == 0 || need > cap) {
        return 0;
    }
    prefix_len = zri_envelope_encode_prefix(buf, cap, envelope);
    if (prefix_len == 0) {
        return 0;
    }
    if (envelope->payload_len > 0) {
        memcpy(buf + prefix_len, envelope->payload, envelope->payload_len);
    }
    return prefix_len + envelope->payload_len;
}

static int envelope_decode_ext(const zri_frame_t *frame, zri_envelope_t *view)
{
    zri_ext_iter_t it;
    zri_ext_t e;
    int rc;

    zri_ext_iter_init(&it, frame->ext, frame->header.ext_len);
    while ((rc = zri_ext_iter_next(&it, &e)) == 1) {
        switch (e.tag) {
        case ZRI_EXT_ROUTE:
            view->route = (const char *)e.value;
            view->route_len = e.len;
            break;
        case ZRI_EXT_METADATA:
            view->metadata = e.value;
            view->metadata_len = e.len;
            break;
        case ZRI_EXT_STATUS:
            if (e.len < ZRI_EXT_STATUS_HDR_LEN) {
                return ERR_INVALID;
            }
            view->status = (int)zri_get_u32(e.value);
            view->status_msg = (const char *)(e.value + ZRI_EXT_STATUS_HDR_LEN);
            view->status_msg_len = (uint16_t)(e.len - ZRI_EXT_STATUS_HDR_LEN);
            break;
        case ZRI_EXT_STREAM:
            if (e.len < ZRI_EXT_STREAM_LEN) {
                return ERR_INVALID;
            }
            view->stream_flags =
                (uint8_t)(STREAM_FLAG_STREAM |
                          (e.value[0] & (STREAM_FIRST | STREAM_LAST | STREAM_CANCEL)));
            view->stream_id = frame->header.stream_id;
            view->stream_offset = zri_get_u64(e.value + 1);
            view->stream_total_size = zri_get_u64(e.value + 9);
            break;
        case ZRI_EXT_DEADLINE:
            if (e.len < ZRI_EXT_DEADLINE_LEN) {
                return ERR_INVALID;
            }
            view->timeout_ms = zri_get_u32(e.value);
            break;
        default:
            /* 未知扩展按长度跳过，保证前后兼容。 */
            break;
        }
    }
    return rc < 0 ? ERR_INVALID : OK;
}

int zri_envelope_decode(const uint8_t *buf, size_t len, zri_envelope_t *view)
{
    zri_frame_t frame;
    int rc;

    if (!view) {
        return ERR_INVALID;
    }
    rc = zri_frame_decode(buf, len, &frame);
    if (rc != OK) {
        return rc;
    }
    memset(view, 0, sizeof(*view));
    if (frame.header.frame_type == ZRI_FRAME_DATA) {
        if (frame.header.kind > KIND_EVENT) {
            return ERR_INVALID;
        }
        view->kind = frame.header.kind;
    } else if (frame.header.frame_type == ZRI_FRAME_CONTROL) {
        if (frame.header.kind != ZRI_CTRL_CANCEL) {
            return ERR_INVALID; /* 其余控制语义尚未接入 core */
        }
        view->kind = KIND_CANCEL;
    } else {
        return ERR_INVALID;
    }
    view->encoding = frame.header.encoding;
    view->flags = frame.header.flags;
    view->request_id = frame.header.request_id;
    view->stream_id = frame.header.stream_id;
    rc = envelope_decode_ext(&frame, view);
    if (rc != OK) {
        return rc;
    }
    view->payload = frame.payload;
    view->payload_len = frame.header.payload_len;
    return OK;
}

void zri_envelope_to_message(const zri_envelope_t *envelope,
                             char *route_buf,
                             size_t route_cap,
                             zri_message_t *out)
{
    size_t route_len;

    if (!envelope || !route_buf || route_cap == 0 || !out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    route_len = envelope->route_len;
    if (route_len >= route_cap) {
        route_len = route_cap - 1;
    }
    if (route_len > 0 && envelope->route) {
        memcpy(route_buf, envelope->route, route_len);
    } else {
        route_len = 0;
    }
    route_buf[route_len] = '\0';

    out->kind = envelope->kind;
    out->flags = envelope->flags;
    out->status = envelope->status;
    out->request_id = envelope->request_id;
    out->timeout_ms = envelope->timeout_ms;
    out->metadata = envelope->metadata;
    out->metadata_len = envelope->metadata_len;
    out->status_msg = envelope->status_msg;
    out->status_msg_len = envelope->status_msg_len;
    out->route = route_buf;
    out->payload.data = envelope->payload;
    out->payload.len = envelope->payload_len;
    out->payload.encoding = envelope->encoding;
    if (envelope->stream_flags & STREAM_FLAG_STREAM) {
        out->has_stream = 1;
        out->stream.flags = envelope->stream_flags & (STREAM_FIRST | STREAM_LAST | STREAM_CANCEL);
        out->stream.stream_id = envelope->stream_id;
        out->stream.offset = envelope->stream_offset;
        out->stream.total_size = envelope->stream_total_size;
    }
}
