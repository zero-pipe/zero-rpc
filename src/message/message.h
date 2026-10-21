#ifndef MESSAGE_H
#define MESSAGE_H

/*
 * 统一消息上下文（provider-neutral message context）。
 *
 * 一条消息在传输层与 core 之间只以一个 zri_message_t 传递：入站方向由传输层
 * 填充（含对端地址与回程 token），出站方向由 core 填充（含目标地址）。它取代
 * 了此前散落在 transport / node / service / reply 各处的一长串位置参数：
 *
 *   (ip, port, token, kind, status, msg_id, route, payload, stream)
 *   -> (const zri_message_t *)
 *
 * 分层约定不变：传输层只搬运字节，kind/status/route/request_id 由 core 解释，
 * payload 对框架透明。zri_peer_token_t 是传输私有的回程标识，仅用于 reply。
 */

#include <types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *zri_peer_token_t;

typedef struct zri_message {
    uint8_t kind;         /* KIND_REQUEST / RESPONSE / EVENT */
    uint8_t flags;        /* MSG_FLAG_*；随 envelope 透传 */
    int status;           /* RESPONSE：应用状态码，或带框架标志时的框架错误码 */
    uint32_t request_id;  /* 请求/响应关联 ID */
    uint32_t timeout_ms;  /* 请求剩余超时毫秒；0 = 无 deadline */
    const void *metadata; /* metadata TLV 字节；可为 NULL */
    uint16_t metadata_len;
    const char *status_msg; /* 状态消息；可为 NULL */
    uint16_t status_msg_len;
    const char *route;    /* "service.method"；可为 NULL */
    payload_t payload;    /* 对框架透明的业务字节 */
    stream_meta_t stream; /* 仅 has_stream 非 0 时有效 */
    int has_stream;

    /* 寻址 / 对端上下文：入站为来源，出站为目标。 */
    const char *peer_ip;
    uint16_t peer_port;
    zri_peer_token_t peer_token; /* 入站回程标识；出站为 NULL */
} zri_message_t;

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_H */
