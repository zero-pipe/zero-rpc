#ifndef ZRPC_METRICS_H
#define ZRPC_METRICS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zrpc_metrics {
    uint64_t requests_sent;
    uint64_t responses_received;
    uint64_t requests_failed;
    uint64_t timeouts;
    uint64_t retries;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t udp_packets_sent;
    uint64_t udp_packets_received;
    uint64_t udp_retransmissions;
    uint64_t udp_nacks_sent;
    uint64_t udp_reassembly_expired;
    uint64_t tcp_frames_sent;
    uint64_t tcp_frames_received;
    uint64_t backpressure_events;
    /* RTCP 报告（RFC 3550，仅 UDP 传输使用） */
    uint64_t rtcp_sr_sent;      /* 本端发出的 Sender Report 数 */
    uint64_t rtcp_rr_sent;      /* 本端发出的 Receiver Report 数 */
    uint64_t rtcp_sr_received;  /* 收到的 Sender Report 数 */
    uint64_t rtcp_rr_received;  /* 收到的 Receiver Report 数 */
    uint32_t rtcp_rtt_ms;       /* 最近一次由对端报告块算得的 RTT */
    uint32_t rtcp_jitter_ms;    /* 本端接收抖动的当前估计 */
    int32_t rtcp_remote_lost;   /* 对端报告的本端流累计丢包 */
} zrpc_metrics_t;

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_METRICS_H */
