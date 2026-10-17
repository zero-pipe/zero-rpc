#ifndef METRICS_H
#define METRICS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 节点累计指标 ---- */
typedef struct metrics {
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
    uint64_t rtcp_bye_sent;     /* 本端发出的 BYE 数 */
    uint64_t rtcp_bye_received; /* 收到的 BYE 数 */
    uint32_t rtcp_rtt_ms;       /* 最近一次由对端报告块算得的 RTT */
    uint32_t rtcp_jitter_ms;    /* 本端接收抖动的当前估计 */
    int32_t rtcp_remote_lost;   /* 对端报告的本端流累计丢包 */
} metrics_t;

/* ---- 对端链路质量 ---- */
/*
 * 传输无关的对端链路统计，见 node_peer_stats()。
 *
 * UDP 由 RTCP SR/RR 填充；TCP 等无链路反馈的传输查询返回 ERR_NOTFOUND。
 * 这里只暴露“链路质量”语义，RTP/RTCP 的具体报文类型不进入公共抽象。
 */
typedef struct peer_stats {
    uint32_t local_id;          /* 本端链路标识（UDP: SSRC） */
    uint32_t remote_id;         /* 对端链路标识（UDP: SSRC） */
    uint32_t rtt_ms;            /* 往返时延估计 */
    uint32_t jitter_ms;         /* 接收抖动估计 */
    int32_t lost;               /* 对端报告的累计丢包 */
    uint64_t feedback_sent;     /* 本端发出的链路反馈包数（UDP: SR+RR） */
    uint64_t feedback_received; /* 本端收到的链路反馈包数（UDP: SR+RR） */
} peer_stats_t;

/* ---- 旧版兼容结构 ---- */
/*
 * 旧版 UDP 专有查询结构保留，避免改变既有调用方的内存布局。新代码应使用
 * peer_stats_t / node_peer_stats()。
 */
typedef struct rtcp_peer_stats {
    uint32_t ssrc_local;
    uint32_t ssrc_remote;
    uint32_t rtt_ms;
    uint32_t jitter_ms;
    int32_t remote_lost;
    uint64_t sr_sent;
    uint64_t rr_sent;
    uint64_t sr_received;
    uint64_t rr_received;
} rtcp_peer_stats_t;

#ifdef __cplusplus
}
#endif

#endif /* METRICS_H */
