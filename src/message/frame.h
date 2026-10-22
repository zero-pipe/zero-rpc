#ifndef ZRI_FRAME_H
#define ZRI_FRAME_H

/*
 * Wire frame v2: fixed header + TLV extensions + payload.
 *
 * 固定头只放每帧都需要的字段，status/metadata/stream/deadline 等附加语义一律
 * 放进扩展，新增语义无需改动固定头。所有整数均为大端。
 *
 * Header (28 bytes):
 *   [0..1]   magic 'Z','R'
 *   [2]      version (2)
 *   [3]      frame_type: DATA(0) / CONTROL(1)
 *   [4]      kind: DATA=REQUEST/RESPONSE/EVENT; CONTROL=opcode
 *   [5]      encoding
 *   [6]      flags
 *   [7]      reserved (0)
 *   [8..11]  stream_id
 *   [12..15] request_id
 *   [16..19] ext_len
 *   [20..23] payload_len
 *   [24..27] checksum = CRC32(header with this field zeroed + extensions)
 *
 * Extension TLV: [tag u16][len u16][value len]
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZRI_FRAME_MAGIC0 0x5Au /* 'Z' */
#define ZRI_FRAME_MAGIC1 0x52u /* 'R' */
#define ZRI_FRAME_VERSION 2u
#define ZRI_FRAME_HDR_LEN 28u
#define ZRI_FRAME_OFF_CHECKSUM 24u

#define ZRI_FRAME_DATA 0u
#define ZRI_FRAME_CONTROL 1u

/* 数据帧 kind */
#define ZRI_KIND_REQUEST 0u
#define ZRI_KIND_RESPONSE 1u
#define ZRI_KIND_EVENT 2u

/* 控制帧 opcode */
#define ZRI_CTRL_CANCEL 0u
#define ZRI_CTRL_GOAWAY 1u
#define ZRI_CTRL_PING 2u

/* 标准扩展 tag；0x0100 起为应用自定义段。 */
#define ZRI_EXT_ROUTE 0x0001u
#define ZRI_EXT_METADATA 0x0002u
#define ZRI_EXT_STATUS 0x0003u
#define ZRI_EXT_STREAM 0x0004u
#define ZRI_EXT_DEADLINE 0x0005u
#define ZRI_EXT_APP_BASE 0x0100u

#define ZRI_EXT_TLV_HDR_LEN 4u
/* STATUS: status(i32) + message */
#define ZRI_EXT_STATUS_HDR_LEN 4u
/* STREAM: flags(u8) + offset(u64) + total_size(u64) */
#define ZRI_EXT_STREAM_LEN 17u
/* DEADLINE: timeout_ms(u32) */
#define ZRI_EXT_DEADLINE_LEN 4u

typedef struct zri_frame_header {
    uint8_t frame_type;
    uint8_t kind;
    uint8_t encoding;
    uint8_t flags;
    uint32_t stream_id;
    uint32_t request_id;
    uint32_t ext_len;
    uint32_t payload_len;
} zri_frame_header_t;

/* 解码后的 wire 视图：header + 指向 ext/payload 的借用指针。 */
typedef struct zri_frame {
    zri_frame_header_t header;
    const uint8_t *ext;
    const uint8_t *payload;
} zri_frame_t;

typedef struct zri_ext {
    uint16_t tag;
    uint16_t len;
    const uint8_t *value;
} zri_ext_t;

typedef struct zri_ext_iter {
    const uint8_t *base;
    size_t len;
    size_t off;
} zri_ext_iter_t;

/* 扩展构造器：把 TLV 顺序写入调用方缓冲区（不含固定头）。 */
typedef struct zri_ext_set {
    uint8_t *buf;
    size_t cap;
    size_t len;
} zri_ext_set_t;

void zri_ext_iter_init(zri_ext_iter_t *it, const void *ext, size_t ext_len);
/* 返回 1 取到一项，0 迭代结束，-1 扩展格式非法。 */
int zri_ext_iter_next(zri_ext_iter_t *it, zri_ext_t *out);
/* 返回 1 找到，0 未找到，-1 格式非法。 */
int zri_ext_get(const void *ext, size_t ext_len, uint16_t tag, zri_ext_t *out);

void zri_ext_set_init(zri_ext_set_t *set, void *buf, size_t cap);
/* 返回 1 成功，0 容量不足/非法（此时 set->len 不变）。 */
int zri_ext_set_add(zri_ext_set_t *set, uint16_t tag, const void *value, size_t len);

/* 头部已经位于 buf，ext 紧随其后（buf + HDR_LEN）；补写头部与 CRC。 */
int zri_frame_write_header(uint8_t *buf, size_t ext_len, const zri_frame_header_t *header);
size_t zri_frame_size(size_t ext_len, size_t payload_len);
size_t zri_frame_encode_prefix(uint8_t *buf,
                               size_t cap,
                               const zri_frame_header_t *header,
                               const void *ext,
                               size_t ext_len);
size_t zri_frame_encode(uint8_t *buf,
                        size_t cap,
                        const zri_frame_header_t *header,
                        const void *ext,
                        size_t ext_len,
                        const void *payload,
                        size_t payload_len);
int zri_frame_decode(const uint8_t *buf, size_t len, zri_frame_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ZRI_FRAME_H */
