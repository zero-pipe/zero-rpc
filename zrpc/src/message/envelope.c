#include "envelope.h"

#include "util.h"

#include <string.h>

size_t zri_envelope_size(size_t route_len, size_t payload_len) {
    return (size_t)ZRPC_ENVELOPE_HDR_LEN + route_len + payload_len;
}

size_t zri_envelope_encode(uint8_t *buf, size_t cap, const zri_envelope_t *envelope) {
    size_t route_len;
    size_t need;
    uint8_t *p;

    if (!buf || !envelope) {
        return 0;
    }
    route_len = envelope->route ? strlen(envelope->route) : 0;
    if (route_len > 0xffffu || (envelope->payload_len > 0 && !envelope->payload)) {
        return 0;
    }
    need = zri_envelope_size(route_len, envelope->payload_len);
    if (need > cap) {
        return 0;
    }
    p = buf;
    p[0] = envelope->kind;
    p[1] = envelope->encoding;
    p[2] = envelope->stream_flags;
    p[3] = 0;
    zri_put_u32(p + 4, (uint32_t)envelope->status);
    zri_put_u32(p + 8, envelope->request_id);
    zri_put_u32(p + 12, envelope->stream_id);
    zri_put_u64(p + 16, envelope->stream_offset);
    zri_put_u64(p + 24, envelope->stream_total_size);
    zri_put_u16(p + 32, (uint16_t)route_len);
    zri_put_u16(p + 34, 0);
    p += ZRPC_ENVELOPE_HDR_LEN;
    if (route_len > 0) {
        memcpy(p, envelope->route, route_len);
        p += route_len;
    }
    if (envelope->payload_len > 0) {
        memcpy(p, envelope->payload, envelope->payload_len);
    }
    return need;
}

int zri_envelope_decode(const uint8_t *buf, size_t len, zri_envelope_t *view) {
    uint16_t route_len;
    if (!buf || !view || len < ZRPC_ENVELOPE_HDR_LEN) {
        return ZRPC_ERR_INVALID;
    }
    route_len = zri_get_u16(buf + 32);
    if ((size_t)ZRPC_ENVELOPE_HDR_LEN + route_len > len) {
        return ZRPC_ERR_INVALID;
    }
    view->kind = buf[0];
    view->encoding = buf[1];
    view->stream_flags = buf[2];
    view->status = (int)zri_get_u32(buf + 4);
    view->request_id = zri_get_u32(buf + 8);
    view->stream_id = zri_get_u32(buf + 12);
    view->stream_offset = zri_get_u64(buf + 16);
    view->stream_total_size = zri_get_u64(buf + 24);
    view->route = (const char *)(buf + ZRPC_ENVELOPE_HDR_LEN);
    view->route_len = route_len;
    view->payload = buf + ZRPC_ENVELOPE_HDR_LEN + route_len;
    view->payload_len = len - ZRPC_ENVELOPE_HDR_LEN - route_len;
    return ZRPC_OK;
}
