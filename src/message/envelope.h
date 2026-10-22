#ifndef ENVELOPE_H
#define ENVELOPE_H

#include <types.h>

#include <stddef.h>
#include <stdint.h>

#include "message.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Provider-neutral RPC envelope. All integer fields are big-endian. */
#define ENVELOPE_MAGIC0 0x5Au /* 'Z' */
#define ENVELOPE_MAGIC1 0x52u /* 'R' */
#define ENVELOPE_VERSION 1u
#define ENVELOPE_OFF_CHECKSUM 46
#define ENVELOPE_HDR_LEN 50

/* RPC kind (envelope-level semantics). */
#define KIND_REQUEST 0
#define KIND_RESPONSE 1
#define KIND_EVENT 2
#define KIND_CANCEL 3 /* 客户端取消：靠 request_id + 对端定位在途调用 */

#define STREAM_FLAG_STREAM 0x80
#define STREAM_FLAG_FIRST STREAM_FIRST
#define STREAM_FLAG_LAST STREAM_LAST
#define STREAM_FLAG_CANCEL STREAM_CANCEL

/*
 * Single envelope descriptor for both encoding and decoding (no separate
 * "view" type): encode reads the fields, decode fills them. route_len is an
 * output on decode and ignored on encode (derived from strlen(route)).
 *
 * Wire header (big-endian), 50 bytes:
 *   [0..1]   magic        ('Z','R')
 *   [2]      version      (当前 1；不匹配直接拒绝)
 *   [3]      kind
 *   [4]      encoding
 *   [5]      stream_flags
 *   [6]      flags        (MSG_FLAG_*)
 *   [7]      reserved     (0)
 *   [8..11]  status       (应用状态码；仅 RESPONSE 有意义)
 *   [12..15] request_id
 *   [16..19] stream_id
 *   [20..27] stream_offset
 *   [28..35] stream_total_size
 *   [36..37] route_len
 *   [38..39] metadata_len
 *   [40..43] timeout_ms   (请求剩余超时；0 = 无 deadline；CANCEL 时忽略)
 *   [44..45] status_msg_len
 *   [46..49] checksum     (CRC32 over header+route+metadata+status_msg with
 *                          these 4 bytes zeroed; payload 由传输层负责)
 *
 * 布局：header + route + metadata + status_msg + payload。
 */
typedef struct zri_envelope {
    uint8_t kind;
    uint8_t encoding;
    uint8_t stream_flags;
    uint8_t flags;        /* MSG_FLAG_*；随包透传 */
    int status;
    uint32_t request_id;
    uint32_t stream_id;
    uint64_t stream_offset;
    uint64_t stream_total_size;
    uint32_t timeout_ms; /* 请求剩余超时毫秒；0 = 无 deadline */
    const char *route;
    uint16_t route_len;   /* decode: filled; encode: ignored */
    const void *metadata; /* metadata TLV 字节；可为 NULL */
    uint16_t metadata_len;
    const char *status_msg; /* 状态消息；可为 NULL */
    uint16_t status_msg_len;
    const void *payload;
    size_t payload_len;
} zri_envelope_t;

size_t zri_envelope_size(size_t route_len,
                         size_t metadata_len,
                         size_t status_msg_len,
                         size_t payload_len);
size_t zri_envelope_encode(uint8_t *buf, size_t cap, const zri_envelope_t *envelope);
int zri_envelope_decode(const uint8_t *buf, size_t len, zri_envelope_t *view);

/*
 * 把解码后的 envelope 转成统一消息上下文。route 会被拷贝到调用方提供的
 * route_buf（保持 NUL 结尾），其余字段直接借用 envelope 所在缓冲。
 */
void zri_envelope_to_message(const zri_envelope_t *envelope,
                             char *route_buf,
                             size_t route_cap,
                             zri_message_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ENVELOPE_H */
