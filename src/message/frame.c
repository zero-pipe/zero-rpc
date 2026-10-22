#include "frame.h"

#include "bytes.h"
#include "crc32.h"

#include <types.h>

#include <string.h>

/*
 * CRC 覆盖固定头（checksum 字段视作 0）与全部扩展，不含 payload；payload 的
 * 完整性由传输层负责（UDP 校验和 / TCP）。这样发送端可以只编码前缀，payload
 * 零拷贝直发。
 */
static uint32_t nb_frame_checksum(const uint8_t *buf, size_t ext_len)
{
    static const uint8_t zero[4] = { 0, 0, 0, 0 };
    uint32_t crc = nb_crc32(buf, NB_FRAME_OFF_CHECKSUM, 0);

    crc = nb_crc32(zero, sizeof(zero), crc);
    if (ext_len > 0) {
        crc = nb_crc32(buf + NB_FRAME_HDR_LEN, ext_len, crc);
    }
    return crc;
}

void nb_ext_iter_init(nb_ext_iter_t *it, const void *ext, size_t ext_len)
{
    if (!it) {
        return;
    }
    it->base = (const uint8_t *)ext;
    it->len = ext ? ext_len : 0;
    it->off = 0;
}

int nb_ext_iter_next(nb_ext_iter_t *it, nb_ext_t *out)
{
    uint16_t tag;
    uint16_t len;

    if (!it || !out) {
        return -1;
    }
    if (it->off >= it->len) {
        return 0;
    }
    if (it->len - it->off < NB_EXT_TLV_HDR_LEN) {
        return -1;
    }
    tag = nb_get_u16(it->base + it->off);
    len = nb_get_u16(it->base + it->off + 2);
    if (it->len - it->off - NB_EXT_TLV_HDR_LEN < len) {
        return -1;
    }
    out->tag = tag;
    out->len = len;
    out->value = it->base + it->off + NB_EXT_TLV_HDR_LEN;
    it->off += NB_EXT_TLV_HDR_LEN + len;
    return 1;
}

int nb_ext_get(const void *ext, size_t ext_len, uint16_t tag, nb_ext_t *out)
{
    nb_ext_iter_t it;
    nb_ext_t e;
    int rc;

    nb_ext_iter_init(&it, ext, ext_len);
    while ((rc = nb_ext_iter_next(&it, &e)) == 1) {
        if (e.tag == tag) {
            if (out) {
                *out = e;
            }
            return 1;
        }
    }
    return rc;
}

void nb_ext_set_init(nb_ext_set_t *set, void *buf, size_t cap)
{
    if (!set) {
        return;
    }
    set->buf = (uint8_t *)buf;
    set->cap = buf ? cap : 0;
    set->len = 0;
}

int nb_ext_set_add(nb_ext_set_t *set, uint16_t tag, const void *value, size_t len)
{
    size_t need;

    if (!set || !set->buf || (len > 0 && !value) || len > 0xffffu) {
        return 0;
    }
    need = (size_t)NB_EXT_TLV_HDR_LEN + len;
    if (set->len > set->cap || need > set->cap - set->len) {
        return 0;
    }
    nb_put_u16(set->buf + set->len, tag);
    nb_put_u16(set->buf + set->len + 2, (uint16_t)len);
    if (len > 0) {
        memcpy(set->buf + set->len + NB_EXT_TLV_HDR_LEN, value, len);
    }
    set->len += need;
    return 1;
}

int nb_frame_write_header(uint8_t *buf, size_t ext_len, const nb_frame_header_t *header)
{
    if (!buf || !header) {
        return 0;
    }
    buf[0] = (uint8_t)NB_FRAME_MAGIC0;
    buf[1] = (uint8_t)NB_FRAME_MAGIC1;
    buf[2] = (uint8_t)NB_FRAME_VERSION;
    buf[3] = header->frame_type;
    buf[4] = header->kind;
    buf[5] = header->encoding;
    buf[6] = header->flags;
    buf[7] = 0;
    nb_put_u32(buf + 8, header->stream_id);
    nb_put_u32(buf + 12, header->request_id);
    nb_put_u32(buf + 16, (uint32_t)ext_len);
    nb_put_u32(buf + 20, header->payload_len);
    nb_put_u32(buf + NB_FRAME_OFF_CHECKSUM, 0);
    nb_put_u32(buf + NB_FRAME_OFF_CHECKSUM, nb_frame_checksum(buf, ext_len));
    return 1;
}

size_t nb_frame_size(size_t ext_len, size_t payload_len)
{
    return (size_t)NB_FRAME_HDR_LEN + ext_len + payload_len;
}

size_t nb_frame_encode_prefix(uint8_t *buf,
                               size_t cap,
                               const nb_frame_header_t *header,
                               const void *ext,
                               size_t ext_len)
{
    if (!buf || !header || (ext_len > 0 && !ext)) {
        return 0;
    }
    if (ext_len > 0xffffffffu || (size_t)NB_FRAME_HDR_LEN + ext_len > cap) {
        return 0;
    }
    if (ext_len > 0) {
        memcpy(buf + NB_FRAME_HDR_LEN, ext, ext_len);
    }
    if (!nb_frame_write_header(buf, ext_len, header)) {
        return 0;
    }
    return (size_t)NB_FRAME_HDR_LEN + ext_len;
}

size_t nb_frame_encode(uint8_t *buf,
                        size_t cap,
                        const nb_frame_header_t *header,
                        const void *ext,
                        size_t ext_len,
                        const void *payload,
                        size_t payload_len)
{
    size_t prefix_len;

    if (!buf || !header || (ext_len > 0 && !ext) || (payload_len > 0 && !payload)) {
        return 0;
    }
    if (payload_len > 0xffffffffu ||
        nb_frame_size(ext_len, payload_len) > cap) {
        return 0;
    }
    prefix_len = nb_frame_encode_prefix(buf, cap, header, ext, ext_len);
    if (prefix_len == 0) {
        return 0;
    }
    if (payload_len > 0) {
        memcpy(buf + prefix_len, payload, payload_len);
    }
    return prefix_len + payload_len;
}

int nb_frame_decode(const uint8_t *buf, size_t len, nb_frame_t *out)
{
    nb_ext_iter_t it;
    nb_ext_t e;
    uint32_t ext_len;
    uint32_t payload_len;
    int rc;

    if (!buf || !out || len < NB_FRAME_HDR_LEN) {
        return NB_ERR_INVALID;
    }
    if (buf[0] != (uint8_t)NB_FRAME_MAGIC0 || buf[1] != (uint8_t)NB_FRAME_MAGIC1) {
        return NB_ERR_INVALID;
    }
    if (buf[2] != (uint8_t)NB_FRAME_VERSION) {
        return NB_ERR_INVALID;
    }
    ext_len = nb_get_u32(buf + 16);
    payload_len = nb_get_u32(buf + 20);
    if ((size_t)NB_FRAME_HDR_LEN + ext_len > len ||
        (size_t)NB_FRAME_HDR_LEN + ext_len + payload_len != len) {
        return NB_ERR_INVALID;
    }
    if (nb_get_u32(buf + NB_FRAME_OFF_CHECKSUM) != nb_frame_checksum(buf, ext_len)) {
        return NB_ERR_INVALID;
    }
    /* 完整走一遍扩展，尽早拒绝截断/越界的 TLV。 */
    nb_ext_iter_init(&it, buf + NB_FRAME_HDR_LEN, ext_len);
    while ((rc = nb_ext_iter_next(&it, &e)) == 1) {
        /* 只做格式校验，内容由调用方解释。 */
    }
    if (rc < 0) {
        return NB_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    out->header.frame_type = buf[3];
    out->header.kind = buf[4];
    out->header.encoding = buf[5];
    out->header.flags = buf[6];
    out->header.stream_id = nb_get_u32(buf + 8);
    out->header.request_id = nb_get_u32(buf + 12);
    out->header.ext_len = ext_len;
    out->header.payload_len = payload_len;
    out->ext = buf + NB_FRAME_HDR_LEN;
    out->payload = buf + NB_FRAME_HDR_LEN + ext_len;
    return NB_OK;
}
