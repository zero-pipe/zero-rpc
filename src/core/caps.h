#ifndef CAPS_H
#define CAPS_H

/*
 * 容量与上限常量：跨模块共享的编译期配置。
 *
 * 单独成头，使传输层等模块无需引入 node 内部结构即可拿到这些常量。
 */

#include "framing.h" /* RTP_HDR_LEN / FRAG_HDR_LEN */
#include <stdint.h>
#include <stdlib.h>

/* ---- 消息与路由 ---- */
#define MAX_ROUTE 128
#define MAX_MSG 1048576u /* 1 MiB */
#define MAX_MSG_LIMIT (64u * 1024u * 1024u)

/* ---- UDP 分片与背压 ---- */
#define FRAG_DEFAULT 1200 /* UDP 默认每分片 payload 字节 */
#define FRAG_MIN 256
#define FRAG_MAX 8192
#define BACKPRESSURE_DEFAULT (4u * 1024u * 1024u)

/* ---- 每节点容量 ---- */
#define MAX_PEERS 64
#define DISC_TARGET_MAX 32
#define PEER_MAX_SERVICES 32
#define CODEC_MAX 8
#define MAX_RECV_STREAMS 64
#define STREAM_IDLE_TIMEOUT_MS 15000

/* ---- 传输扇出 ---- */
#define IO_MAX 8
#define PEERS_PER_IO 64
#define TCP_LINKS_PER_IO 64

/* ---- UDP 可靠性参数 ---- */
#define REASM_TIMEOUT_MS 3000
#define NACK_INTERVAL_MS 10
#define MAX_NACK_SEQS 64
#define RTO_INIT_MS 150
#define MAX_RETX 5
#define TX_BATCH_MAX 32
#define RX_BATCH_MAX 16
/* 拥塞窗口下限：保证 16 KiB（14 个分片）始终能一次批量发出。 */
#define CC_BURST_MIN 16
#define MAX_FRAGMENTS (MAX_MSG / FRAG_MIN)
#define RTX_SLOT_MAX (RTP_HDR_LEN + FRAG_HDR_LEN + FRAG_MAX)

/* ---- RTCP 报告 ---- */
#define RTCP_INTERVAL_MS 1000 /* SR/RR 上报周期 */

/* ---- 周期 ---- */
#define TICK_INTERVAL_MS 2

/* per-poller 缓冲池：每档最多缓存的空闲块数，避免池无界增长 */
#define BUF_POOL_MAX_PER_BUCKET 32

/* Runtime-overridable tick period: env TICK_INTERVAL_MS wins, else the
 * compile-time default. A longer tick cuts timer overhead (measured ~10% more
 * throughput at saturation) but coarsens UDP NACK/RTX and TCP backlog flush,
 * hence configurable rather than hardcoded. */
static inline uint32_t zri_tick_interval_ms(void) {
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
