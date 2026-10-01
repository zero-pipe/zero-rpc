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
} zrpc_metrics_t;

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_METRICS_H */
