#ifndef ZRI_CONFIG_DEFAULTS_H
#define ZRI_CONFIG_DEFAULTS_H

/*
 * 配置默认值 / 范围 / 语义总表（单一真源）。
 *
 * config 解析、校验与 node 装配都引用这里，避免同一默认值散落在多处导致漂移。
 * 表中“0”多数表示“使用默认”，由 node_create 兜底为下表值。
 *
 * 项                        默认               范围                 语义
 * ------------------------- ------------------ -------------------- --------------------
 * io_threads                0(=硬件并发)       0..ZRI_MAX_IO        poller 分片线程数
 * handler_threads           0(关)              0..ZRI_MAX_HANDLER   worker 线程池
 * handler_max_concurrency   0(不限)            0..N                 worker 在途上限
 * max_inflight              65536              0..N                 客户端 pending 上限
 * max_streams               64                 0..MAX_RECV_STREAMS  并发接收流上限
 * peer_rate_limit_pps       0(不限)            0..N                 每对端入站限速
 * frag_bytes                1200               256..8192           UDP 分片
 * max_msg_bytes             1 MiB              ..MAX_MSG_LIMIT      单条消息上限
 * backpressure_bytes        4 MiB              0..N                 出站高水位
 * rtcp_interval_ms          1000               0..N                 UDP RTCP 周期
 * hello_interval_ms         3000               0..N                 发现 HELLO 周期
 * node_ttl_ms               9000               0..N                 对端过期
 * metrics.sample_every      1                  1..N                 延迟采样
 */

#include "caps.h" /* MAX_MSG / MAX_RECV_STREAMS / IO_MAX */

#define ZRI_DEFAULT_NODE_NAME "rpc"
#define ZRI_DEFAULT_BIND_HOST "0.0.0.0"
#define ZRI_DEFAULT_DISCOVERY_HOST "255.255.255.255"
#define ZRI_DEFAULT_HELLO_INTERVAL_MS 3000u
#define ZRI_DEFAULT_NODE_TTL_MS 9000u
#define ZRI_DEFAULT_METRICS_SAMPLE_EVERY 1u

/* 校验上界：超过即判非法（对应旧 config.c 的硬编码）。 */
#define ZRI_MAX_IO_THREADS 1024u
#define ZRI_MAX_HANDLER_THREADS 1024u

#endif /* ZRI_CONFIG_DEFAULTS_H */
