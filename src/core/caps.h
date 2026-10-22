#ifndef CAPS_H
#define CAPS_H

/*
 * 跨模块共享的编译期容量/上限常量。
 *
 * 传输相关常量（分片/背压/扇出/可靠性/RTCP）见 transport/transport_caps.h；
 * 本头只保留跨模块（core / message / router / discovery）共用的常量。
 */

#include "transport_caps.h" /* FRAG_MIN/MAX 等，供 MAX_FRAGMENTS 使用 */

#include <types.h> /* NB_SERVICE_NAME_MAX / NB_METHOD_NAME_MAX */
#include <stdint.h>
#include <stdlib.h>

/* ---- 消息与路由 ---- */
/* "service.method" 的最大缓冲区长度（含结尾 '\0'）。 */
#define MAX_ROUTE (NB_SERVICE_NAME_MAX + NB_METHOD_NAME_MAX + 2)
#define MAX_MSG 1048576u /* 1 MiB */
#define MAX_MSG_LIMIT (64u * 1024u * 1024u)
/*
 * frame 固定头 + 扩展相对 payload 的最大额外字节：route/metadata/status 值，
 * 加上 5 个 TLV 头、status code、stream/deadline 定长值与 28B 固定头（80B 余量）。
 */
#define MAX_ENVELOPE_EXTRA (MAX_ROUTE + NB_METADATA_WIRE_MAX + NB_STATUS_MSG_MAX + 80u)
/* 单条消息分片数硬上限：覆盖 max_msg 上限 + 最小分片的最坏情况，仅拦截异常值。 */
#define MAX_FRAGMENTS (MAX_MSG_LIMIT / FRAG_MIN)

/* ---- 每节点容量 ---- */
#define MAX_PEERS 64
#define DEFAULT_MAX_INFLIGHT 65536u /* 客户端未完成调用上限（防无界增长） */
#define DISC_TARGET_MAX 32
#define PEER_MAX_SERVICES 32
#define CODEC_MAX 8
#define MAX_RECV_STREAMS 64
#define STREAM_IDLE_TIMEOUT_MS 15000
#define RATE_LIMIT_SLOTS 256
#define RATE_WINDOW_MS 1000

/* ---- 传输扇出（跨模块：node 出站分片也用到 IO_MAX） ---- */
#define IO_MAX 8

/* ---- 周期 ---- */
#define TICK_INTERVAL_MS 2

/* per-poller 缓冲池：每档最多缓存的空闲块数，避免池无界增长 */
#define BUF_POOL_MAX_PER_BUCKET 32

/*
 * Runtime-overridable tick period: env TICK_INTERVAL_MS wins, else the
 * compile-time default. A longer tick cuts timer overhead (measured ~10% more
 * throughput at saturation) but coarsens UDP NACK/RTX and TCP backlog flush,
 * hence configurable rather than hardcoded.
 */
static inline uint32_t nb_tick_interval_ms(void)
{
    static int inited = 0;
    static uint32_t value = TICK_INTERVAL_MS;
    if (!inited) {
        const char *env = getenv("TICK_INTERVAL_MS");
        if (env && env[0]) {
            int v = atoi(env);
            if (v > 0 && v <= 1000) {
                value = (uint32_t)v;
            }
        }
        inited = 1;
    }
    return value;
}

#endif /* CAPS_H */
