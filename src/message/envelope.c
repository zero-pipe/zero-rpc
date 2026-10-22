#include "envelope.h"

#include "bytes.h"

#include <string.h>

static size_t nb_envelope_route_len(const nb_envelope_t *envelope)
{
    return envelope->route ? strlen(envelope->route) : 0;
}

static int nb_envelope_has_stream(const nb_envelope_t *envelope)
{
    return (envelope->stream_flags & STREAM_FLAG_STREAM) != 0;
}

/* 扩展区字节数（不含固定头与 payload）。 */
static size_t nb_envelope_ext_size(const nb_envelope_t *envelope, size_t route_len)
{
    size_t n = 0;

    if (route_len > 0) {
        n += NB_EXT_TLV_HDR_LEN + route_len;
    }
    if (envelope->metadata_len > 0) {
        n += NB_EXT_TLV_HDR_LEN + envelope->metadata_len;
    }
    if (envelope->status != 0 || envelope->status_msg_len > 0) {
        n += NB_EXT_TLV_HDR_LEN + NB_EXT_STATUS_HDR_LEN + envelope->status_msg_len;
    }
    if (nb_envelope_has_stream(envelope)) {
        n += NB_EXT_TLV_HDR_LEN + NB_EXT_STREAM_LEN;
    }
    if (envelope->timeout_ms > 0) {
        n += NB_EXT_TLV_HDR_LEN + NB_EXT_DEADLINE_LEN;
    }
    return n;
}

static size_t nb_ext_reserve(uint8_t *buf, size_t cap, size_t off, uint16_t tag, size_t len)
{
    if (len > 0xffffu || off > cap || cap - off < NB_EXT_TLV_HDR_LEN + len) {
        return 0;
    }
    nb_put_u16(buf + off, tag);
    nb_put_u16(buf + off + 2, (uint16_t)len);
    return off + NB_EXT_TLV_HDR_LEN;
}

static size_t nb_envelope_build_ext(const nb_envelope_t *envelope,
                                 size_t route_len,
                                 uint8_t *buf,
                                 size_t cap)
{
    size_t off = 0;

    if (route_len > 0) {
        off = nb_ext_reserve(buf, cap, off, NB_EXT_ROUTE, route_len);
        if (off == 0) {
            return 0;
        }
        memcpy(buf + off, envelope->route, route_len);
        off += route_len;
    }
    if (envelope->metadata_len > 0) {
        off = nb_ext_reserve(buf, cap, off, NB_EXT_METADATA, envelope->metadata_len);
        if (off == 0) {
            return 0;
        }
        memcpy(buf + off, envelope->metadata, envelope->metadata_len);
        off += envelope->metadata_len;
    }
    if (envelope->status != 0 || envelope->status_msg_len > 0) {
        off = nb_ext_reserve(
            buf, cap, off, NB_EXT_STATUS, NB_EXT_STATUS_HDR_LEN + envelope->status_msg_len);
        if (off == 0) {
            return 0;
        }
        nb_put_u32(buf + off, (uint32_t)envelope->status);
        off += NB_EXT_STATUS_HDR_LEN;
        if (envelope->status_msg_len > 0) {
            memcpy(buf + off, envelope->status_msg, envelope->status_msg_len);
            off += envelope->status_msg_len;
        }
    }
    if (nb_envelope_has_stream(envelope)) {
        off = nb_ext_reserve(buf, cap, off, NB_EXT_STREAM, NB_EXT_STREAM_LEN);
        if (off == 0) {
            return 0;
        }
        buf[off] = (uint8_t)(envelope->stream_flags & (NB_STREAM_FIRST | NB_STREAM_LAST | NB_STREAM_CANCEL));
        nb_put_u64(buf + off + 1, envelope->stream_offset);
        nb_put_u64(buf + off + 9, envelope->stream_total_size);
        off += NB_EXT_STREAM_LEN;
    }
    if (envelope->timeout_ms > 0) {
        off = nb_ext_reserve(buf, cap, off, NB_EXT_DEADLINE, NB_EXT_DEADLINE_LEN);
        if (off == 0) {
            return 0;
        }
        nb_put_u32(buf + off, envelope->timeout_ms);
        off += NB_EXT_DEADLINE_LEN;
    }
    return off;
}

static void nb_envelope_fill_header(const nb_envelope_t *envelope,
                                 size_t ext_len,
                                 nb_frame_header_t *header)
{
    memset(header, 0, sizeof(*header));
    if (envelope->kind >= KIND_CANCEL && envelope->kind <= KIND_PING) {
        header->frame_type = NB_FRAME_CONTROL;
        header->kind = (uint8_t)(envelope->kind - KIND_CANCEL);
    } else {
        header->frame_type = NB_FRAME_DATA;
        header->kind = envelope->kind;
    }
    header->encoding = envelope->encoding;
    header->flags = envelope->flags;
    header->stream_id = envelope->stream_id;
    header->request_id = envelope->request_id;
    header->ext_len = (uint32_t)ext_len;
    header->payload_len = (uint32_t)envelope->payload_len;
}

size_t nb_envelope_size(const nb_envelope_t *envelope)
{
    size_t route_len;

    if (!envelope) {
        return 0;
    }
    route_len = nb_envelope_route_len(envelope);
    return (size_t)ENVELOPE_HDR_LEN + nb_envelope_ext_size(envelope, route_len) +
           envelope->payload_len;
}

size_t nb_envelope_encode_prefix(uint8_t *buf, size_t cap, const nb_envelope_t *envelope)
{
    nb_frame_header_t header;
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
    route_len = nb_envelope_route_len(envelope);
    if (route_len > 0xffffu || cap < ENVELOPE_HDR_LEN) {
        return 0;
    }
    expect = nb_envelope_ext_size(envelope, route_len);
    ext_len = nb_envelope_build_ext(
        envelope, route_len, buf + ENVELOPE_HDR_LEN, cap - ENVELOPE_HDR_LEN);
    if (ext_len != expect) {
        return 0;
    }
    nb_envelope_fill_header(envelope, ext_len, &header);
    if (!nb_frame_write_header(buf, ext_len, &header)) {
        return 0;
    }
    return (size_t)ENVELOPE_HDR_LEN + ext_len;
}

size_t nb_envelope_encode(uint8_t *buf, size_t cap, const nb_envelope_t *envelope)
{
    size_t need;
    size_t prefix_len;

    if (!buf || !envelope) {
        return 0;
    }
    if (envelope->payload_len > 0 && !envelope->payload) {
        return 0;
    }
    need = nb_envelope_size(envelope);
    if (need == 0 || need > cap) {
        return 0;
    }
    prefix_len = nb_envelope_encode_prefix(buf, cap, envelope);
    if (prefix_len == 0) {
        return 0;
    }
    if (envelope->payload_len > 0) {
        memcpy(buf + prefix_len, envelope->payload, envelope->payload_len);
    }
    return prefix_len + envelope->payload_len;
}

static int nb_envelope_decode_ext(const nb_frame_t *frame, nb_envelope_t *view)
{
    nb_ext_iter_t it;
    nb_ext_t e;
    int rc;

    nb_ext_iter_init(&it, frame->ext, frame->header.ext_len);
    while ((rc = nb_ext_iter_next(&it, &e)) == 1) {
        switch (e.tag) {
        case NB_EXT_ROUTE:
            view->route = (const char *)e.value;
            view->route_len = e.len;
            break;
        case NB_EXT_METADATA:
            view->metadata = e.value;
            view->metadata_len = e.len;
            break;
        case NB_EXT_STATUS:
            if (e.len < NB_EXT_STATUS_HDR_LEN) {
                return NB_ERR_INVALID;
            }
            view->status = (int)nb_get_u32(e.value);
            view->status_msg = (const char *)(e.value + NB_EXT_STATUS_HDR_LEN);
            view->status_msg_len = (uint16_t)(e.len - NB_EXT_STATUS_HDR_LEN);
            break;
        case NB_EXT_STREAM:
            if (e.len < NB_EXT_STREAM_LEN) {
                return NB_ERR_INVALID;
            }
            view->stream_flags =
                (uint8_t)(STREAM_FLAG_STREAM |
                          (e.value[0] & (NB_STREAM_FIRST | NB_STREAM_LAST | NB_STREAM_CANCEL)));
            view->stream_id = frame->header.stream_id;
            view->stream_offset = nb_get_u64(e.value + 1);
            view->stream_total_size = nb_get_u64(e.value + 9);
            break;
        case NB_EXT_DEADLINE:
            if (e.len < NB_EXT_DEADLINE_LEN) {
                return NB_ERR_INVALID;
            }
            view->timeout_ms = nb_get_u32(e.value);
            break;
        default:
            /* 未知扩展按长度跳过，保证前后兼容。 */
            break;
        }
    }
    return rc < 0 ? NB_ERR_INVALID : NB_OK;
}

int nb_envelope_decode(const uint8_t *buf, size_t len, nb_envelope_t *view)
{
    nb_frame_t frame;
    int rc;

    if (!view) {
        return NB_ERR_INVALID;
    }
    rc = nb_frame_decode(buf, len, &frame);
    if (rc != NB_OK) {
        return rc;
    }
    memset(view, 0, sizeof(*view));
    if (frame.header.frame_type == NB_FRAME_DATA) {
        if (frame.header.kind > KIND_EVENT) {
            return NB_ERR_INVALID;
        }
        view->kind = frame.header.kind;
    } else if (frame.header.frame_type == NB_FRAME_CONTROL) {
        if (frame.header.kind > NB_CTRL_PING) {
            return NB_ERR_INVALID;
        }
        view->kind = (uint8_t)(KIND_CANCEL + frame.header.kind);
    } else {
        return NB_ERR_INVALID;
    }
    view->encoding = frame.header.encoding;
    view->flags = frame.header.flags;
    view->request_id = frame.header.request_id;
    view->stream_id = frame.header.stream_id;
    rc = nb_envelope_decode_ext(&frame, view);
    if (rc != NB_OK) {
        return rc;
    }
    view->payload = frame.payload;
    view->payload_len = frame.header.payload_len;
    return NB_OK;
}

void nb_envelope_to_message(const nb_envelope_t *envelope,
                             char *route_buf,
                             size_t route_cap,
                             nb_message_t *out)
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
        out->stream.flags = envelope->stream_flags & (NB_STREAM_FIRST | NB_STREAM_LAST | NB_STREAM_CANCEL);
        out->stream.stream_id = envelope->stream_id;
        out->stream.offset = envelope->stream_offset;
        out->stream.total_size = envelope->stream_total_size;
    }
}
