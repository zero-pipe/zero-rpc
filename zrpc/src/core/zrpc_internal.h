#ifndef ZRPC_INTERNAL_H
#define ZRPC_INTERNAL_H

#include <zrpc/zrpc.h>
#include <ztk/ztk.h>

#include <stddef.h>
#include <stdint.h>

#include "zrpc_discovery.h"
#include "zrpc_envelope.h"
#include "zrpc_registry.h"
#include "zrpc_registry_backend.h"
#include "zrpc_router.h"
#include "zrpc_transport.h"
#include "zrpc_rx_window.h"
#include "zrpc_reasm.h"
#include "zrpc_tx_ring.h"
#include "zrpc_util.h"
#include "zrpc_wire.h"

/* ---- 容量与上限 ---- */
#define ZRPC_FRAG_DEFAULT 1200 /* UDP 默认每分片 payload 字节 */
#define ZRPC_FRAG_MIN 256
#define ZRPC_FRAG_MAX 8192
#define ZRPC_MAX_ROUTE 128
#define ZRPC_MAX_MSG 1048576u /* 1 MiB */
#define ZRPC_MAX_MSG_LIMIT (64u * 1024u * 1024u)
#define ZRPC_BACKPRESSURE_DEFAULT (4u * 1024u * 1024u)
#define ZRPC_MAX_PEERS 64
#define ZRPC_DISC_TARGET_MAX 32
#define ZRPC_PEER_MAX_SERVICES 32
#define ZRPC_PENDING_MAX 256
#define ZRPC_IO_MAX 8
#define ZRPC_PEERS_PER_IO 64
#define ZRPC_CODEC_MAX 8
#define ZRPC_TCP_LINKS_PER_IO 64
#define ZRPC_MAX_RECV_STREAMS 64
#define ZRPC_STREAM_IDLE_TIMEOUT_MS 15000

/* UDP 可靠性参数（窗口/重组/重传环的容量在各自模块头文件里定义） */
#define ZRPC_REASM_TIMEOUT_MS 3000
#define ZRPC_NACK_INTERVAL_MS 10
#define ZRPC_MAX_NACK_SEQS 64
#define ZRPC_RTO_INIT_MS 150
#define ZRPC_MAX_RETX 5
#define ZRPC_TX_BATCH_MAX 32
#define ZRPC_RX_BATCH_MAX 16
#define ZRPC_MAX_FRAGMENTS (ZRPC_MAX_MSG / ZRPC_FRAG_MIN)
#define ZRPC_RTX_SLOT_MAX (ZRPC_RTP_HDR_LEN + ZRPC_FRAG_HDR_LEN + ZRPC_FRAG_MAX)

#define ZRPC_TICK_INTERVAL_MS 2

/* ---- 发现的 mesh 对端 ---- */
typedef struct zrpc_peer {
    int used;
    uint32_t ip;
    char ip_s[ZRPC_ENDPOINT_MAX];
    zrpc_endpoint_t endpoints[ZRPC_MAX_BINDINGS];
    size_t endpoint_count;
    uint32_t node_id;
    uint64_t last_seen_ms;
    int service_count;
    char services[ZRPC_PEER_MAX_SERVICES][ZRPC_SERVICE_NAME_MAX];
} zrpc_peer_t;

/* 显式发现目标（单播 HELLO） */
typedef struct zrpc_disc_target {
    int used;
    char ip[ZRPC_ENDPOINT_MAX];
    uint16_t port;
} zrpc_disc_target_t;

typedef struct zrpc_method {
    struct zrpc_method *next;
    char name[ZRPC_METHOD_NAME_MAX];
    zrpc_handler_fn fn;
    zrpc_chunk_handler_fn stream_fn;
    void *user;
} zrpc_method_t;

struct zrpc_service {
    struct zrpc_service *next;
    zrpc_node_t *node;
    char name[ZRPC_SERVICE_NAME_MAX];
    zrpc_method_t *methods;
};

struct zrpc_proxy {
    zrpc_node_t *node;
    uint8_t payload_encoding;
    uint32_t prefer_stream_threshold;
    int has_endpoint;
    char endpoint_ip[ZRPC_ENDPOINT_MAX];
    char endpoint_scheme[ZRPC_SCHEME_MAX];
    uint16_t endpoint_port;
    zrpc_transport_kind_t endpoint_transport;
};

struct zrpc_stream {
    zrpc_proxy_t *proxy;
    zrpc_node_t *node;
    uint64_t request_id;
    uint32_t stream_id;
    char service[ZRPC_SERVICE_NAME_MAX];
    char method[ZRPC_METHOD_NAME_MAX];
    char ip[ZRPC_ENDPOINT_MAX];
    char scheme[ZRPC_SCHEME_MAX];
    uint16_t port;
    zrpc_transport_kind_t transport;
    zrpc_response_fn cb;
    zrpc_response_chunk_fn chunk_cb;
    zrpc_stream_done_fn done_cb;
    void *user;
    int bidi;
    uint64_t timeout_ms;
    uint64_t next_offset;
    int opened;
    int sent_any;
    int send_closed;
    int closed;
};

struct zrpc_call {
    zrpc_node_t *node;
    zrpc_transport_t *transport;
    zrpc_peer_token_t token;
    char peer_ip[ZRPC_ENDPOINT_MAX];
    uint16_t peer_port;
    uint32_t msg_id;
    int replied;
    int stream_owned;
};

typedef struct zrpc_recv_stream {
    int used;
    uint32_t request_id;
    char peer_ip[ZRPC_ENDPOINT_MAX];
    uint16_t peer_port;
    zrpc_call_t *call;
    struct zrpc_method *method;
    char service[ZRPC_SERVICE_NAME_MAX];
    char method_name[ZRPC_METHOD_NAME_MAX];
    uint64_t last_ms;
    uint64_t bytes;
} zrpc_recv_stream_t;

typedef struct zrpc_pending {
    int used;
    zrpc_node_t *node;
    uint64_t request_id;
    zrpc_response_fn cb;
    void *user;
    uint64_t deadline_ms;
    /* RTO 重传（弱网整包丢失时 NACK 无从触发，需超时重发请求） */
    int has_req;
    int streaming;
    zrpc_response_chunk_fn chunk_cb;
    zrpc_stream_done_fn done_cb;
    void *stream_user;
    uint64_t expected_offset;
    uint32_t bp_retries;
    uint64_t next_retx_ms;
    uint32_t retries;
    char service[ZRPC_SERVICE_NAME_MAX];
    char method[ZRPC_METHOD_NAME_MAX];
    char ip[ZRPC_ENDPOINT_MAX];
    uint16_t port;
    zrpc_transport_kind_t transport;
    char scheme[ZRPC_SCHEME_MAX];
    uint8_t *data;
    size_t len;
    uint8_t encoding;
} zrpc_pending_t;

/* codec 注册项 */
typedef struct zrpc_codec_entry {
    struct zrpc_codec_entry *next;
    zrpc_codec_t codec;
} zrpc_codec_entry_t;

struct zrpc_node {
    zrpc_node_config_t cfg;
    char name[ZRPC_NAME_MAX];
    uint32_t node_id;
    char bind_host[ZRPC_ENDPOINT_MAX];
    char discovery_host[ZRPC_ENDPOINT_MAX];
    uint16_t discovery_port;
    unsigned io_threads;
    uint32_t hello_interval_ms;
    uint32_t node_ttl_ms;
    uint16_t frag_bytes;
    uint32_t max_msg_bytes;
    uint32_t bp_high_water;
    uint8_t drop_percent; /* 测试用：UDP 出站丢包百分比 */

    ztk_poller_pool *pool;
    ztk_poller *poller; /* io 0，用于发现/客户端调用 */
    zrpc_transport_t *transports[ZRPC_MAX_BINDINGS];
    size_t transport_count;
    /* Compatibility alias for the first binding. */
    zrpc_transport_t *transport;
    ztk_socket *disc_sock;
    ztk_timer *hello_timer;
    ztk_timer *node_timer;

    zrpc_service_t *services;
    zrpc_peer_t peers[ZRPC_MAX_PEERS];
    zrpc_disc_target_t disc_targets[ZRPC_DISC_TARGET_MAX];
    zrpc_pending_t pending[ZRPC_PENDING_MAX];
    zrpc_recv_stream_t recv_streams[ZRPC_MAX_RECV_STREAMS];
    zrpc_codec_entry_t *codecs;

    zrpc_node_mode_t mode;
    ztk_mutex *route_lock;
    zrpc_route_t *routes;
    uint64_t route_gen;
    zrpc_registry_t registry;
    const zrpc_registry_backend_t *registry_backend;
    zrpc_metrics_t metrics;

    ztk_mutex *id_lock;
    uint64_t next_request_id;
    int started;
};

/* ---- 跨模块接口 ---- */

/* node.c */
uint64_t zrpc_now_ms(void);
void zrpc_node_on_message(zrpc_node_t *node, const char *ip, uint16_t port, zrpc_peer_token_t token,
                          zrpc_transport_t *transport, uint8_t kind, uint32_t msg_id, const char *route,
                          const zrpc_payload_t *payload, const zrpc_stream_meta_t *stream);
int zrpc_node_send(zrpc_node_t *node, const char *ip, uint16_t port, uint8_t kind, uint32_t msg_id,
                   zrpc_transport_kind_t transport_kind, const char *scheme,
                   const char *route, const zrpc_payload_t *payload,
                   const zrpc_stream_meta_t *stream);

/* service.c */
void zrpc_service_handle_request(zrpc_node_t *node, zrpc_peer_token_t token, const char *ip,
                                 zrpc_transport_t *transport, uint16_t port, uint32_t msg_id,
                                 const char *route,
                                 const zrpc_payload_t *payload);
void zrpc_service_handle_stream_chunk(zrpc_node_t *node, zrpc_peer_token_t token, const char *ip,
                                      zrpc_transport_t *transport, uint16_t port, uint32_t msg_id,
                                      const char *route, const zrpc_stream_meta_t *stream,
                                      const zrpc_payload_t *payload);
void zrpc_service_expire_streams(zrpc_node_t *node, uint64_t now_ms);

/* proxy.c */
int zrpc_proxy_resolve(zrpc_proxy_t *proxy, const char *service, char *ip_out, size_t ip_cap,
                       uint16_t *port_out, zrpc_transport_kind_t *transport_out,
                       char *scheme_out, size_t scheme_cap, int prefer_stream);
void zrpc_pending_retx(zrpc_node_t *node, zrpc_pending_t *p);

/* pending.c */
zrpc_pending_t *zrpc_pending_add(zrpc_node_t *node, uint64_t id, zrpc_response_fn cb, void *user,
                                 uint64_t timeout_ms);
zrpc_pending_t *zrpc_pending_find(zrpc_node_t *node, uint64_t id);
void zrpc_pending_invoke(zrpc_pending_t *p, int status, const zrpc_response_t *resp);
void zrpc_pending_release(zrpc_pending_t *p);
void zrpc_pending_tick(zrpc_node_t *node, uint64_t now_ms);

#endif /* ZRPC_INTERNAL_H */
