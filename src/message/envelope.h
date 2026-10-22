#ifndef ENVELOPE_H
#define ENVELOPE_H

#include <types.h>

#include <stddef.h>
#include <stdint.h>

#include "frame.h"
#include "message.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Provider-neutral RPC envelope: the semantic view used by core. On the wire it
 * is encoded as a v2 frame (fixed header + TLV extensions + payload); see
 * frame.h. All integer fields are big-endian.
 */
#define ENVELOPE_MAGIC0 NB_FRAME_MAGIC0
#define ENVELOPE_MAGIC1 NB_FRAME_MAGIC1
#define ENVELOPE_VERSION NB_FRAME_VERSION
#define ENVELOPE_OFF_CHECKSUM NB_FRAME_OFF_CHECKSUM
#define ENVELOPE_HDR_LEN NB_FRAME_HDR_LEN

/*
 * 扩展相对固定头之外的附加字节上限：5 个标准 TLV 头（route/metadata/status/
 * stream/deadline）、status code、stream 与 deadline 的定长值。
 */
#define ENVELOPE_EXT_OVERHEAD 64u

/* RPC kind (envelope-level semantics). */
#define KIND_REQUEST 0
#define KIND_RESPONSE 1
#define KIND_EVENT 2
#define KIND_CANCEL 3 /* 客户端取消：靠 request_id + 对端定位在途调用 */
#define KIND_GOAWAY 4 /* 对端不再接收新请求 */
#define KIND_PING 5   /* 控制面心跳；收到后原样回显 */

#define STREAM_FLAG_STREAM 0x80
#define STREAM_FLAG_FIRST NB_STREAM_FIRST
#define STREAM_FLAG_LAST NB_STREAM_LAST
#define STREAM_FLAG_CANCEL NB_STREAM_CANCEL

/*
 * Single envelope descriptor for both encoding and decoding (no separate
 * "view" type): encode reads the fields, decode fills them. route_len is an
 * output on decode and ignored on encode (derived from strlen(route)).
 *
 * Wire layout (v2): header + extensions(route/metadata/status/stream/deadline)
 * + payload. KIND_CANCEL is encoded as a CONTROL frame; every other kind is a
 * DATA frame. Absent optional extensions decode to zero/NULL.
 */
typedef struct nb_envelope {
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
} nb_envelope_t;

/* 编码后的总字节数（header + extensions + payload）。 */
size_t nb_envelope_size(const nb_envelope_t *envelope);
/*
 * 只编码 header + extensions（payload_len 仍写入头部），payload 由调用方紧随
 * 其后发送。返回前缀字节数，0 表示失败。
 */
size_t nb_envelope_encode_prefix(uint8_t *buf, size_t cap, const nb_envelope_t *envelope);
size_t nb_envelope_encode(uint8_t *buf, size_t cap, const nb_envelope_t *envelope);
int nb_envelope_decode(const uint8_t *buf, size_t len, nb_envelope_t *view);

/*
 * 把解码后的 envelope 转成统一消息上下文。route 会被拷贝到调用方提供的
 * route_buf（保持 NUL 结尾），其余字段直接借用 envelope 所在缓冲。
 */
void nb_envelope_to_message(const nb_envelope_t *envelope,
                             char *route_buf,
                             size_t route_cap,
                             nb_message_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ENVELOPE_H */
