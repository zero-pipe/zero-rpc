#ifndef NB_FRAME_H
#define NB_FRAME_H

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

#define NB_FRAME_MAGIC0 0x5Au /* 'Z' */
#define NB_FRAME_MAGIC1 0x52u /* 'R' */
#define NB_FRAME_VERSION 2u
#define NB_FRAME_HDR_LEN 28u
#define NB_FRAME_OFF_CHECKSUM 24u

#define NB_FRAME_DATA 0u
#define NB_FRAME_CONTROL 1u

/* 数据帧 kind */
#define NB_KIND_REQUEST 0u
#define NB_KIND_RESPONSE 1u
#define NB_KIND_EVENT 2u

/* 控制帧 opcode */
#define NB_CTRL_CANCEL 0u
#define NB_CTRL_GOAWAY 1u
#define NB_CTRL_PING 2u

/* 标准扩展 tag；0x0100 起为应用自定义段。 */
#define NB_EXT_ROUTE 0x0001u
#define NB_EXT_METADATA 0x0002u
#define NB_EXT_STATUS 0x0003u
#define NB_EXT_STREAM 0x0004u
#define NB_EXT_DEADLINE 0x0005u
#define NB_EXT_APP_BASE 0x0100u

#define NB_EXT_TLV_HDR_LEN 4u
/* STATUS: status(i32) + message */
#define NB_EXT_STATUS_HDR_LEN 4u
/* STREAM: flags(u8) + offset(u64) + total_size(u64) */
#define NB_EXT_STREAM_LEN 17u
/* DEADLINE: timeout_ms(u32) */
#define NB_EXT_DEADLINE_LEN 4u

typedef struct nb_frame_header {
    uint8_t frame_type;
    uint8_t kind;
    uint8_t encoding;
    uint8_t flags;
    uint32_t stream_id;
    uint32_t request_id;
    uint32_t ext_len;
    uint32_t payload_len;
} nb_frame_header_t;

/* 解码后的 wire 视图：header + 指向 ext/payload 的借用指针。 */
typedef struct nb_frame {
    nb_frame_header_t header;
    const uint8_t *ext;
    const uint8_t *payload;
} nb_frame_t;

typedef struct nb_ext {
    uint16_t tag;
    uint16_t len;
    const uint8_t *value;
} nb_ext_t;

typedef struct nb_ext_iter {
    const uint8_t *base;
    size_t len;
    size_t off;
} nb_ext_iter_t;

/* 扩展构造器：把 TLV 顺序写入调用方缓冲区（不含固定头）。 */
typedef struct nb_ext_set {
    uint8_t *buf;
    size_t cap;
    size_t len;
} nb_ext_set_t;

void nb_ext_iter_init(nb_ext_iter_t *it, const void *ext, size_t ext_len);
/* 返回 1 取到一项，0 迭代结束，-1 扩展格式非法。 */
int nb_ext_iter_next(nb_ext_iter_t *it, nb_ext_t *out);
/* 返回 1 找到，0 未找到，-1 格式非法。 */
int nb_ext_get(const void *ext, size_t ext_len, uint16_t tag, nb_ext_t *out);

void nb_ext_set_init(nb_ext_set_t *set, void *buf, size_t cap);
/* 返回 1 成功，0 容量不足/非法（此时 set->len 不变）。 */
int nb_ext_set_add(nb_ext_set_t *set, uint16_t tag, const void *value, size_t len);

/* 头部已经位于 buf，ext 紧随其后（buf + HDR_LEN）；补写头部与 CRC。 */
int nb_frame_write_header(uint8_t *buf, size_t ext_len, const nb_frame_header_t *header);
size_t nb_frame_size(size_t ext_len, size_t payload_len);
size_t nb_frame_encode_prefix(uint8_t *buf,
                               size_t cap,
                               const nb_frame_header_t *header,
                               const void *ext,
                               size_t ext_len);
size_t nb_frame_encode(uint8_t *buf,
                        size_t cap,
                        const nb_frame_header_t *header,
                        const void *ext,
                        size_t ext_len,
                        const void *payload,
                        size_t payload_len);
int nb_frame_decode(const uint8_t *buf, size_t len, nb_frame_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NB_FRAME_H */
