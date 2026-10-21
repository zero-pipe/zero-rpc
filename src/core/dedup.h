#ifndef ZRI_DEDUP_H
#define ZRI_DEDUP_H

/*
 * 服务端请求去重 / 响应重放缓存。
 *
 * UDP 的整包重传（pending RTO）会重复投递同一 request_id。若 handler 非幂等，
 * 重复执行就是 bug。这里按 (request_id, peer) 记录在途/已完成请求：
 *   - 命中“已完成”：直接重放缓存响应，不再执行 handler；
 *   - 命中“在途”  ：丢弃重复（原响应到达后客户端自会拿到）；
 *   - 未命中      ：登记在途，交给上层正常执行。
 *
 * 只对 UDP unary 生效（TCP 可靠无需去重）。分片 + 固定槽 + TTL + 字节预算，
 * 访问用每分片一把锁，热路径无全局争用。
 */

#include <types.h>

#include <ntk.h>

#include <stddef.h>
#include <stdint.h>

#include "message.h"
#include "transport_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZRI_DEDUP_SHARDS 16
#define ZRI_DEDUP_SLOTS_PER_SHARD 16
#define ZRI_DEDUP_TTL_MS 10000u
#define ZRI_DEDUP_MAX_BYTES (8u * 1024u * 1024u)

typedef struct zri_dedup_slot {
    int used;
    int completed;
    uint32_t request_id;
    char peer_ip[ENDPOINT_MAX];
    uint16_t peer_port;
    uint64_t expires_ms;
    uint64_t last_ms;
    int status;
    uint8_t encoding;
    uint8_t *payload;
    size_t payload_len;
    uint8_t *metadata;
    uint16_t metadata_len;
    char *message;
    uint16_t message_len;
} zri_dedup_slot_t;

typedef struct zri_dedup_shard {
    ntk_mutex *lock;
    size_t bytes;
    zri_dedup_slot_t slots[ZRI_DEDUP_SLOTS_PER_SHARD];
} zri_dedup_shard_t;

typedef struct zri_dedup {
    zri_dedup_shard_t shards[ZRI_DEDUP_SHARDS];
} zri_dedup_t;

void zri_dedup_init(zri_dedup_t *d);
void zri_dedup_cleanup(zri_dedup_t *d);

/*
 * state: 0 = 新请求（上层需正常处理），1 = 已重放缓存响应，2 = 在途重复（丢弃）。
 */
int zri_dedup_begin(zri_dedup_t *d, zri_transport_t *transport, const zri_message_t *req, int *state);
/* 记录最终响应，供后续重复请求重放。 */
void zri_dedup_complete(zri_dedup_t *d,
                        zri_transport_t *transport,
                        const zri_message_t *req,
                        int status,
                        const payload_t *payload,
                        const void *metadata,
                        uint16_t metadata_len,
                        const char *message,
                        uint16_t message_len);
/* 处理失败时撤销登记，允许重传重试。 */
void zri_dedup_abort(zri_dedup_t *d, const zri_message_t *req);
void zri_dedup_expire(zri_dedup_t *d, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZRI_DEDUP_H */
