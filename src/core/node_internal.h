#ifndef NODE_INTERNAL_H
#define NODE_INTERNAL_H

/*
 * 节点内部结构：节点与 codec 注册项。
 *
 * 这是各子系统的“组合根”：node 内嵌 pending / recv_streams / 发现状态 / 路由 /
 * registry / 传输等存储，因此本头聚合了这些子系统的定义。其它模块应尽量通过
 * 访问器/专用头与本模块交互，而不是各自穿透 node 字段。
 */

#include <types.h>
#include <node.h>
#include <codec.h>
#include <stub.h>
#include <ntk.h>

#include <stddef.h>
#include <stdint.h>

#include "caps.h"
#include "discovery.h"
#include "pending.h"
#include "registry.h"
#include "registry_backend.h"
#include "router.h"
#include "service_internal.h"
#include "transport_internal.h"

/* codec 注册项 */
typedef struct zri_codec_entry {
    struct zri_codec_entry *next;
    codec_t codec;
} zri_codec_entry_t;

struct node {
    node_config_t cfg;
    char name[NAME_MAX];
    uint32_t node_id;
    char bind_host[ENDPOINT_MAX];
    char discovery_host[ENDPOINT_MAX];
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

    ntk_poller_pool *pool;
    ntk_poller *poller; /* io 0，用于发现/客户端调用 */
    zri_transport_t *transports[MAX_BINDINGS];
    size_t transport_count;
    /* Compatibility alias for the first binding. */
    zri_transport_t *transport;
    ntk_timer *hello_timer;
    ntk_timer *node_timer;

    service_t *services;
    zri_discovery_state_t discovery;
    zri_pending_t pending[PENDING_MAX];
    zri_recv_stream_t recv_streams[MAX_RECV_STREAMS];
    zri_codec_entry_t *codecs;

    node_mode_t mode;
    zri_route_table_t routes;
    zri_registry_t registry;
    const zri_registry_backend_t *registry_backend;
    metrics_t metrics;

    ntk_mutex *id_lock;
    uint64_t next_request_id;
    int started;
};

/* node.c */
uint64_t zri_now_ms(void);
void zri_node_on_message(node_t *node, zri_transport_t *transport,
                         const zri_message_t *message);
int zri_node_send(node_t *node, transport_kind_t transport_kind, const char *scheme,
                  const zri_message_t *message);
/* 组装 "service.method" 路由并发送一次调用（stub/proxy/重传共用）。 */
int zri_node_send_call(node_t *node, const stub_target_t *target, const char *service,
                       const char *method, const payload_t *payload, uint8_t kind,
                       uint64_t request_id);
/* 默认解析：静态路由表 / 服务发现（不含 proxy 的端点覆盖）。 */
int zri_node_resolve(node_t *node, const char *service, int prefer_stream,
                      stub_target_t *out);

#endif /* NODE_INTERNAL_H */
