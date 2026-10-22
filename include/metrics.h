#ifndef NB_METRICS_H
#define NB_METRICS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 端到端延迟直方图桶数：<=1,2,5,10,25,50,100,250,500,1000,2500,5000,10000ms，最后一桶为 >10000ms。 */
#define NB_LATENCY_HIST_BUCKETS 14

/* 直方图上界（毫秒），共 NB_LATENCY_HIST_BUCKETS-1 个；最后一桶表示 >=10000ms。 */
extern const uint32_t nb_latency_bounds[NB_LATENCY_HIST_BUCKETS - 1];

/* ---- 节点累计指标 ---- */
typedef struct nb_metrics {
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

    /* 可观测性：当前值（gauge）与端到端延迟分布 */
    uint64_t connections;         /* 当前活跃链路/会话数（UDP session + TCP link） */
    uint64_t inflight_requests;   /* 当前未完成客户端调用数 */
    uint64_t inflight_handlers;   /* 当前未完成 worker/executor 任务数 */
    uint64_t latency_count;       /* 已完成的端到端采样数 */
    uint64_t latency_sum_ms;      /* 延迟总和，用于计算均值 */
    uint64_t latency_hist[NB_LATENCY_HIST_BUCKETS];
    uint64_t latency_p50_ms;      /* 由直方图估算的分位值 */
    uint64_t latency_p90_ms;
    uint64_t latency_p99_ms;
    uint64_t rate_limited;    /* 被 per-peer 入站限速丢弃/回退的请求数 */
    uint64_t streams_rejected; /* 因并发流上限被拒绝的新流数 */
} nb_metrics_t;

/* ---- 对端链路质量 ---- */
/*
 * 传输无关的对端链路统计，见 nb_node_peer_stats()。
 *
 * UDP 由 RTCP SR/RR 填充；TCP 等无链路反馈的传输查询返回 NB_ERR_NOTFOUND。
 * 这里只暴露“链路质量”语义，RTP/RTCP 的具体报文类型不进入公共抽象。
 */
typedef struct nb_peer_stats {
    uint32_t local_id;          /* 本端链路标识（UDP: SSRC） */
    uint32_t remote_id;         /* 对端链路标识（UDP: SSRC） */
    uint32_t rtt_ms;            /* 往返时延估计 */
    uint32_t jitter_ms;         /* 接收抖动估计 */
    int32_t lost;               /* 对端报告的累计丢包 */
    uint64_t feedback_sent;     /* 本端发出的链路反馈包数（UDP: SR+RR） */
    uint64_t feedback_received; /* 本端收到的链路反馈包数（UDP: SR+RR） */
} nb_peer_stats_t;

/* ---- 指标登记表 ----
 *
 * 名称/类型/单位集中登记，导出层（Prometheus 等）只需遍历这张表，不再从
 * 散落的 NB_METRIC_INC 调用点猜测语义。offset 为 nb_metrics_t 内字段偏移。
 */
typedef enum nb_metric_type {
    NB_METRIC_COUNTER = 0,
    NB_METRIC_GAUGE,
    NB_METRIC_HISTOGRAM
} nb_metric_type_t;

typedef struct nb_metric_desc {
    const char *name;
    nb_metric_type_t type;
    const char *unit;
    size_t offset;
} nb_metric_desc_t;

extern const nb_metric_desc_t nb_metric_table[];
extern const size_t nb_metric_table_count;

/* 按名称查登记项；未找到返回 NULL。 */
const nb_metric_desc_t *nb_metric_find(const char *name);
/* 延迟直方图分桶上界（毫秒），返回桶数；bounds 可为 NULL。 */
size_t nb_metric_latency_bounds(const uint32_t **bounds);

#ifdef __cplusplus
}
#endif

#endif /* NB_METRICS_H */
