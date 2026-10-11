#ifndef ZRPC_CAPS_H
#define ZRPC_CAPS_H

/*
 * 容量与上限常量：跨模块共享的编译期配置。
 *
 * 单独成头，使传输层等模块无需引入 node 内部结构即可拿到这些常量。
 */

#include "zrpc_framing.h" /* ZRPC_RTP_HDR_LEN / ZRPC_FRAG_HDR_LEN */

/* ---- 消息与路由 ---- */
#define ZRPC_MAX_ROUTE 128
#define ZRPC_MAX_MSG 1048576u /* 1 MiB */
#define ZRPC_MAX_MSG_LIMIT (64u * 1024u * 1024u)

/* ---- UDP 分片与背压 ---- */
#define ZRPC_FRAG_DEFAULT 1200 /* UDP 默认每分片 payload 字节 */
#define ZRPC_FRAG_MIN 256
#define ZRPC_FRAG_MAX 8192
#define ZRPC_BACKPRESSURE_DEFAULT (4u * 1024u * 1024u)

/* ---- 每节点容量 ---- */
#define ZRPC_MAX_PEERS 64
#define ZRPC_DISC_TARGET_MAX 32
#define ZRPC_PEER_MAX_SERVICES 32
#define ZRPC_PENDING_MAX 256
#define ZRPC_CODEC_MAX 8
#define ZRPC_MAX_RECV_STREAMS 64
#define ZRPC_STREAM_IDLE_TIMEOUT_MS 15000

/* ---- 传输扇出 ---- */
#define ZRPC_IO_MAX 8
#define ZRPC_PEERS_PER_IO 64
#define ZRPC_TCP_LINKS_PER_IO 64

/* ---- UDP 可靠性参数 ---- */
#define ZRPC_REASM_TIMEOUT_MS 3000
#define ZRPC_NACK_INTERVAL_MS 10
#define ZRPC_MAX_NACK_SEQS 64
#define ZRPC_RTO_INIT_MS 150
#define ZRPC_MAX_RETX 5
#define ZRPC_TX_BATCH_MAX 32
#define ZRPC_RX_BATCH_MAX 16
#define ZRPC_MAX_FRAGMENTS (ZRPC_MAX_MSG / ZRPC_FRAG_MIN)
#define ZRPC_RTX_SLOT_MAX (ZRPC_RTP_HDR_LEN + ZRPC_FRAG_HDR_LEN + ZRPC_FRAG_MAX)

/* ---- 周期 ---- */
#define ZRPC_TICK_INTERVAL_MS 2

/* per-poller 缓冲池：每档最多缓存的空闲块数，避免池无界增长 */
#define ZRPC_BUF_POOL_MAX_PER_BUCKET 32

#endif /* ZRPC_CAPS_H */
