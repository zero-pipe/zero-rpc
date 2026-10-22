/* metrics 运行期开关的全局状态（见 atomic.h / config.h）。 */

#include "atomic.h"

#include <metrics.h>

#include <string.h>

volatile int nb_metrics_enabled = 1;
volatile int nb_latency_enabled = 1;
volatile uint32_t nb_metrics_sample_every = 1;

/* 延迟直方图分桶单一真源，node.c / dispatch.c 共用。 */
const uint32_t nb_latency_bounds[NB_LATENCY_HIST_BUCKETS - 1] = {
    1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000
};

#define METRIC(field, type, unit, name) \
    { name, type, unit, offsetof(nb_metrics_t, field) }

/* 名称/类型/单位登记（Prometheus 风格命名）。 */
const nb_metric_desc_t nb_metric_table[] = {
    METRIC(requests_sent, NB_METRIC_COUNTER, "requests", "requests_sent_total"),
    METRIC(responses_received, NB_METRIC_COUNTER, "responses", "responses_received_total"),
    METRIC(requests_failed, NB_METRIC_COUNTER, "requests", "requests_failed_total"),
    METRIC(timeouts, NB_METRIC_COUNTER, "requests", "timeouts_total"),
    METRIC(retries, NB_METRIC_COUNTER, "requests", "retries_total"),
    METRIC(bytes_sent, NB_METRIC_COUNTER, "bytes", "bytes_sent_total"),
    METRIC(bytes_received, NB_METRIC_COUNTER, "bytes", "bytes_received_total"),
    METRIC(udp_packets_sent, NB_METRIC_COUNTER, "packets", "udp_packets_sent_total"),
    METRIC(udp_packets_received, NB_METRIC_COUNTER, "packets", "udp_packets_received_total"),
    METRIC(udp_retransmissions, NB_METRIC_COUNTER, "packets", "udp_retransmissions_total"),
    METRIC(udp_nacks_sent, NB_METRIC_COUNTER, "packets", "udp_nacks_sent_total"),
    METRIC(udp_reassembly_expired, NB_METRIC_COUNTER, "messages", "udp_reassembly_expired_total"),
    METRIC(tcp_frames_sent, NB_METRIC_COUNTER, "frames", "tcp_frames_sent_total"),
    METRIC(tcp_frames_received, NB_METRIC_COUNTER, "frames", "tcp_frames_received_total"),
    METRIC(backpressure_events, NB_METRIC_COUNTER, "events", "backpressure_events_total"),
    METRIC(rtcp_sr_sent, NB_METRIC_COUNTER, "packets", "rtcp_sr_sent_total"),
    METRIC(rtcp_rr_sent, NB_METRIC_COUNTER, "packets", "rtcp_rr_sent_total"),
    METRIC(rtcp_sr_received, NB_METRIC_COUNTER, "packets", "rtcp_sr_received_total"),
    METRIC(rtcp_rr_received, NB_METRIC_COUNTER, "packets", "rtcp_rr_received_total"),
    METRIC(rtcp_bye_sent, NB_METRIC_COUNTER, "packets", "rtcp_bye_sent_total"),
    METRIC(rtcp_bye_received, NB_METRIC_COUNTER, "packets", "rtcp_bye_received_total"),
    METRIC(rtcp_rtt_ms, NB_METRIC_GAUGE, "ms", "rtcp_rtt_ms"),
    METRIC(rtcp_jitter_ms, NB_METRIC_GAUGE, "ms", "rtcp_jitter_ms"),
    METRIC(rtcp_remote_lost, NB_METRIC_GAUGE, "packets", "rtcp_remote_lost"),
    METRIC(connections, NB_METRIC_GAUGE, "connections", "connections"),
    METRIC(inflight_requests, NB_METRIC_GAUGE, "requests", "inflight_requests"),
    METRIC(inflight_handlers, NB_METRIC_GAUGE, "tasks", "inflight_handlers"),
    METRIC(latency_count, NB_METRIC_COUNTER, "samples", "latency_count_total"),
    METRIC(latency_sum_ms, NB_METRIC_COUNTER, "ms", "latency_sum_ms_total"),
    METRIC(latency_hist, NB_METRIC_HISTOGRAM, "ms", "latency_ms"),
    METRIC(latency_p50_ms, NB_METRIC_GAUGE, "ms", "latency_p50_ms"),
    METRIC(latency_p90_ms, NB_METRIC_GAUGE, "ms", "latency_p90_ms"),
    METRIC(latency_p99_ms, NB_METRIC_GAUGE, "ms", "latency_p99_ms"),
    METRIC(rate_limited, NB_METRIC_COUNTER, "requests", "rate_limited_total"),
    METRIC(streams_rejected, NB_METRIC_COUNTER, "streams", "streams_rejected_total"),
};

const size_t nb_metric_table_count = sizeof(nb_metric_table) / sizeof(nb_metric_table[0]);

const nb_metric_desc_t *nb_metric_find(const char *name)
{
    size_t i;

    if (!name) {
        return NULL;
    }
    for (i = 0; i < nb_metric_table_count; i++) {
        if (strcmp(nb_metric_table[i].name, name) == 0) {
            return &nb_metric_table[i];
        }
    }
    return NULL;
}

size_t nb_metric_latency_bounds(const uint32_t **bounds)
{
    if (bounds) {
        *bounds = nb_latency_bounds;
    }
    return NB_LATENCY_HIST_BUCKETS - 1;
}
