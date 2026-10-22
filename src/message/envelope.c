#include "envelope.h"

#include "util.h"

#include <string.h>

size_t zri_envelope_size(size_t route_len,
                         size_t metadata_len,
                         size_t status_msg_len,
                         size_t payload_len)
{
    return (size_t)ENVELOPE_HDR_LEN + route_len + metadata_len + status_msg_len + payload_len;
}

/*
 * CRC32 覆盖控制段（header + route + metadata + status_msg），不含 payload。
 * UDP/TCP 发送时只编码前缀、payload 零拷贝直发，因此校验必须在二者拼接前
 * 就能确定；payload 完整性由传输层（UDP 校验和 / TCP）负责。
 */
static uint32_t envelope_checksum(const uint8_t *buf, size_t len)
{
    static const uint8_t zero[4] = { 0, 0, 0, 0 };
    uint32_t crc = zri_crc32(buf, ENVELOPE_OFF_CHECKSUM, 0);
    crc = zri_crc32(zero, sizeof(zero), crc);
    if (len > (size_t)ENVELOPE_OFF_CHECKSUM + 4u) {
        crc = zri_crc32(buf + ENVELOPE_OFF_CHECKSUM + 4u,
                        len - ENVELOPE_OFF_CHECKSUM - 4u,
                        crc);
    }
    return crc;
}

size_t zri_envelope_encode(uint8_t *buf, size_t cap, const zri_envelope_t *envelope)
{
    size_t route_len;
    size_t need;
    uint8_t *p;

    if (!buf || !envelope) {
        return 0;
    }
    route_len = envelope->route ? strlen(envelope->route) : 0;
    if (route_len > 0xffffu || envelope->metadata_len > 0xffffu ||
        envelope->status_msg_len > 0xffffu || (envelope->payload_len > 0 && !envelope->payload) ||
        (envelope->metadata_len > 0 && !envelope->metadata) ||
        (envelope->status_msg_len > 0 && !envelope->status_msg)) {
        return 0;
    }
    need = zri_envelope_size(
        route_len, envelope->metadata_len, envelope->status_msg_len, envelope->payload_len);
    if (need > cap) {
        return 0;
    }
    p = buf;
    p[0] = (uint8_t)ENVELOPE_MAGIC0;
    p[1] = (uint8_t)ENVELOPE_MAGIC1;
    p[2] = (uint8_t)ENVELOPE_VERSION;
    p[3] = envelope->kind;
    p[4] = envelope->encoding;
    p[5] = envelope->stream_flags;
    p[6] = envelope->flags;
    p[7] = 0;
    zri_put_u32(p + 8, (uint32_t)envelope->status);
    zri_put_u32(p + 12, envelope->request_id);
    zri_put_u32(p + 16, envelope->stream_id);
    zri_put_u64(p + 20, envelope->stream_offset);
    zri_put_u64(p + 28, envelope->stream_total_size);
    zri_put_u16(p + 36, (uint16_t)route_len);
    zri_put_u16(p + 38, envelope->metadata_len);
    zri_put_u32(p + 40, envelope->timeout_ms);
    zri_put_u16(p + 44, envelope->status_msg_len);
    zri_put_u32(p + ENVELOPE_OFF_CHECKSUM, 0);
    p += ENVELOPE_HDR_LEN;
    if (route_len > 0) {
        memcpy(p, envelope->route, route_len);
        p += route_len;
    }
    if (envelope->metadata_len > 0) {
        memcpy(p, envelope->metadata, envelope->metadata_len);
        p += envelope->metadata_len;
    }
    if (envelope->status_msg_len > 0) {
        memcpy(p, envelope->status_msg, envelope->status_msg_len);
        p += envelope->status_msg_len;
    }
    if (envelope->payload_len > 0) {
        memcpy(p, envelope->payload, envelope->payload_len);
    }
    zri_put_u32(buf + ENVELOPE_OFF_CHECKSUM,
                envelope_checksum(
                    buf, (size_t)ENVELOPE_HDR_LEN + route_len + envelope->metadata_len +
                             envelope->status_msg_len));
    return need;
}

int zri_envelope_decode(const uint8_t *buf, size_t len, zri_envelope_t *view)
{
    uint16_t route_len;
    uint16_t metadata_len;
    uint16_t status_msg_len;
    if (!buf || !view || len < ENVELOPE_HDR_LEN) {
        return ERR_INVALID;
    }
    if (buf[0] != (uint8_t)ENVELOPE_MAGIC0 || buf[1] != (uint8_t)ENVELOPE_MAGIC1) {
        return ERR_INVALID;
    }
    if (buf[2] != (uint8_t)ENVELOPE_VERSION) {
        return ERR_INVALID;
    }
    route_len = zri_get_u16(buf + 36);
    metadata_len = zri_get_u16(buf + 38);
    status_msg_len = zri_get_u16(buf + 44);
    if ((size_t)ENVELOPE_HDR_LEN + route_len + metadata_len + status_msg_len > len) {
        return ERR_INVALID;
    }
    if (zri_get_u32(buf + ENVELOPE_OFF_CHECKSUM) !=
        envelope_checksum(
            buf, (size_t)ENVELOPE_HDR_LEN + route_len + metadata_len + status_msg_len)) {
        return ERR_INVALID;
    }
    view->kind = buf[3];
    view->encoding = buf[4];
    view->stream_flags = buf[5];
    view->flags = buf[6];
    view->status = (int)zri_get_u32(buf + 8);
    view->request_id = zri_get_u32(buf + 12);
    view->stream_id = zri_get_u32(buf + 16);
    view->stream_offset = zri_get_u64(buf + 20);
    view->stream_total_size = zri_get_u64(buf + 28);
    view->timeout_ms = zri_get_u32(buf + 40);
    view->route = (const char *)(buf + ENVELOPE_HDR_LEN);
    view->route_len = route_len;
    view->metadata = buf + ENVELOPE_HDR_LEN + route_len;
    view->metadata_len = metadata_len;
    view->status_msg = (const char *)(buf + ENVELOPE_HDR_LEN + route_len + metadata_len);
    view->status_msg_len = status_msg_len;
    view->payload = buf + ENVELOPE_HDR_LEN + route_len + metadata_len + status_msg_len;
    view->payload_len = len - ENVELOPE_HDR_LEN - route_len - metadata_len - status_msg_len;
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
