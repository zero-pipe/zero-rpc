#ifndef ZRPC_NODE_INTERNAL_H
#define ZRPC_NODE_INTERNAL_H

/*
 * 节点内部结构：节点与 codec 注册项。
 *
 * 这是各子系统的“组合根”：node 内嵌 pending / recv_streams / 发现状态 / 路由 /
 * registry / 传输等存储，因此本头聚合了这些子系统的定义。其它模块应尽量通过
 * 访问器/专用头与本模块交互，而不是各自穿透 node 字段。
 */

#include <zrpc/zrpc_types.h>
#include <zrpc/zrpc_node.h>
#include <zrpc/zrpc_codec.h>
#include <zrpc/zrpc_stub.h>
#include <ztk/ztk.h>

#include <stddef.h>
#include <stdint.h>

#include "caps.h"
#include "discovery.h"
#include "pending.h"
#include "registry.h"
#include "registry_backend.h"
#include "router.h"
#include "service_internal.h"
#include "transport.h"

/* codec 注册项 */
typedef struct zri_codec_entry {
    struct zri_codec_entry *next;
    zrpc_codec_t codec;
} zri_codec_entry_t;

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
    uint32_t rtcp_interval_ms;
    int rtcp_disabled;

    ztk_poller_pool *pool;
    ztk_poller *poller; /* io 0，用于发现/客户端调用 */
    zri_transport_t *transports[ZRPC_MAX_BINDINGS];
    size_t transport_count;
    /* Compatibility alias for the first binding. */
    zri_transport_t *transport;
    ztk_timer *hello_timer;
    ztk_timer *node_timer;

    zrpc_service_t *services;
    zri_discovery_state_t discovery;
    zri_pending_t pending[ZRPC_PENDING_MAX];
    zri_recv_stream_t recv_streams[ZRPC_MAX_RECV_STREAMS];
    zri_codec_entry_t *codecs;

    zrpc_node_mode_t mode;
    zri_route_table_t routes;
    zri_registry_t registry;
    const zri_registry_backend_t *registry_backend;
    zrpc_metrics_t metrics;

    ztk_mutex *id_lock;
    uint64_t next_request_id;
    int started;
};

/* node.c */
uint64_t zri_now_ms(void);
void zri_node_on_message(zrpc_node_t *node, const char *ip, uint16_t port, zri_peer_token_t token,
                          zri_transport_t *transport, uint8_t kind, int status, uint32_t msg_id,
                          const char *route, const zrpc_payload_t *payload,
                          const zrpc_stream_meta_t *stream);
int zri_node_send(zrpc_node_t *node, const char *ip, uint16_t port, uint8_t kind, int status,
                   uint32_t msg_id, zrpc_transport_kind_t transport_kind, const char *scheme,
                   const char *route, const zrpc_payload_t *payload,
                   const zrpc_stream_meta_t *stream);
/* 默认解析：静态路由表 / 服务发现（不含 proxy 的端点覆盖）。 */
int zri_node_resolve(zrpc_node_t *node, const char *service, int prefer_stream,
                      zrpc_stub_target_t *out);

#endif /* ZRPC_NODE_INTERNAL_H */
