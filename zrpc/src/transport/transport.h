#ifndef ZRPC_TRANSPORT_H
#define ZRPC_TRANSPORT_H

/*
 * 传输抽象：node 只依赖该接口，不感知 UDP/TCP 差异。
 *
 * 一个传输负责：绑定本地端口、把一条完整 RPC 消息发给某个对端、
 * 把收到的完整消息上抛给 node（zri_node_on_message），以及自身的周期性驱动。
 *
 * payload 在传输层是纯字节；kind/msg_id/route 由上层（core）解释。
 */

#include <zrpc/zrpc_types.h>
#include <zrpc/zrpc_metrics.h>
#include <zrpc/zrpc_transport_provider.h>

#include <ztk/ztk.h>

#include <stdint.h>

#include "caps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zri_transport zri_transport_t;
typedef struct zrpc_node zrpc_node_t;

/* 收到的完整消息上抛给 node 时携带的回程标识（传输私有，仅用于 reply）。 */
typedef void *zri_peer_token_t;

typedef struct zri_transport_ops {
    /* 绑定/监听，成功后 t->port 为实际端口。 */
    int (*start)(zri_transport_t *t);
    void (*stop)(zri_transport_t *t);

    /* 发起（首次）发送：按 ip:port 找到/建立通道。status 仅 RESPONSE 有意义。 */
    int (*send)(zri_transport_t *t, const char *ip, uint16_t port, uint8_t kind, int status,
                uint32_t msg_id, const char *route, const zrpc_payload_t *payload,
                const zrpc_stream_meta_t *stream);

    /* 回复：优先使用 接收（recv）回调给出的 peer_token（回程通道），token 为空时回退到 ip:port。 */
    int (*reply)(zri_transport_t *t, zri_peer_token_t token, const char *ip, uint16_t port,
                 uint8_t kind, int status, uint32_t msg_id, const char *route,
                 const zrpc_payload_t *payload, const zrpc_stream_meta_t *stream);

    /* 查询某对端的 RTCP 统计（可选）：1 = 找到，0 = 无此对端，<0 = 出错。 */
    int (*rtcp_peer)(zri_transport_t *t, const char *ip, uint16_t port,
                     zrpc_rtcp_peer_stats_t *out);
} zri_transport_ops_t;

struct zri_transport {
    const zri_transport_ops_t *ops;
    zrpc_node_t *node;
    zrpc_transport_kind_t kind;
    char scheme[ZRPC_SCHEME_MAX];
    char bind_host[ZRPC_ENDPOINT_MAX];
    uint16_t port;
    const zrpc_transport_provider_t *provider;
    zrpc_transport_instance_t *provider_instance;

    /*
     * 运行期配置快照：由 node 在建立传输时填充。传输层只读这些字段，
     * 不穿透 struct zrpc_node 内部，从而与 core 解耦。
     */
    uint16_t frag_bytes;
    uint32_t max_msg_bytes;
    uint32_t bp_high_water;
    uint8_t drop_percent;
    uint32_t rtcp_interval_ms;
    int rtcp_disabled;
    unsigned io_threads;
    ztk_poller_pool *pool;
    ztk_poller *poller;
    zrpc_metrics_t *metrics;
};

/* 传输层把收到的完整消息上抛给 core 的回调（实现于 node.c）。 */
void zri_node_on_message(zrpc_node_t *node, const char *ip, uint16_t port, zri_peer_token_t token,
                          zri_transport_t *transport, uint8_t kind, int status, uint32_t msg_id,
                          const char *route, const zrpc_payload_t *payload,
                          const zrpc_stream_meta_t *stream);

/* 具体驱动工厂（返回已初始化 ops/kind 的对象，未 start）。 */
zri_transport_t *zri_udp_transport_create(void);
zri_transport_t *zri_tcp_transport_create(void);

/* 便捷包装 */
int zri_transport_start(zri_transport_t *t);
void zri_transport_stop(zri_transport_t *t);
void zri_transport_destroy(zri_transport_t *t);
int zri_transport_send(zri_transport_t *t, const char *ip, uint16_t port, uint8_t kind,
                         int status, uint32_t msg_id, const char *route,
                         const zrpc_payload_t *payload, const zrpc_stream_meta_t *stream);
int zri_transport_reply(zri_transport_t *t, zri_peer_token_t token, const char *ip,
                         uint16_t port, uint8_t kind, int status, uint32_t msg_id,
                         const char *route, const zrpc_payload_t *payload,
                         const zrpc_stream_meta_t *stream);
const zrpc_transport_provider_t *zri_transport_provider_find(const char *scheme);

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_TRANSPORT_H */
