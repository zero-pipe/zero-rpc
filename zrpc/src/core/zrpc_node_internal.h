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

#include "zrpc_caps.h"
#include "zrpc_discovery.h"
#include "zrpc_pending.h"
#include "zrpc_registry.h"
#include "zrpc_registry_backend.h"
#include "zrpc_router.h"
#include "zrpc_service_internal.h"
#include "zrpc_transport.h"

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
    ztk_timer *hello_timer;
    ztk_timer *node_timer;

    zrpc_service_t *services;
    zrpc_discovery_state_t discovery;
    zrpc_pending_t pending[ZRPC_PENDING_MAX];
    zrpc_recv_stream_t recv_streams[ZRPC_MAX_RECV_STREAMS];
    zrpc_codec_entry_t *codecs;

    zrpc_node_mode_t mode;
    zrpc_route_table_t routes;
    zrpc_registry_t registry;
    const zrpc_registry_backend_t *registry_backend;
    zrpc_metrics_t metrics;

    ztk_mutex *id_lock;
    uint64_t next_request_id;
    int started;
};

/* node.c */
uint64_t zrpc_now_ms(void);
void zrpc_node_on_message(zrpc_node_t *node, const char *ip, uint16_t port, zrpc_peer_token_t token,
                          zrpc_transport_t *transport, uint8_t kind, uint32_t msg_id,
                          const char *route, const zrpc_payload_t *payload,
                          const zrpc_stream_meta_t *stream);
int zrpc_node_send(zrpc_node_t *node, const char *ip, uint16_t port, uint8_t kind, uint32_t msg_id,
                   zrpc_transport_kind_t transport_kind, const char *scheme, const char *route,
                   const zrpc_payload_t *payload, const zrpc_stream_meta_t *stream);
/* 默认解析：静态路由表 / 服务发现（不含 proxy 的端点覆盖）。 */
int zrpc_node_resolve(zrpc_node_t *node, const char *service, int prefer_stream,
                      zrpc_stub_target_t *out);

#endif /* ZRPC_NODE_INTERNAL_H */
