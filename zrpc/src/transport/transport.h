#ifndef ZRPC_TRANSPORT_H
#define ZRPC_TRANSPORT_H

/*
 * 传输抽象：node 只依赖该接口，不感知 UDP/TCP 差异。
 *
 * 一个传输负责：绑定本地端口、把一条完整消息发给某个对端、
 * 把收到的完整消息上抛给 core（zri_node_on_message），以及自身的周期性驱动。
 *
 * 消息统一用 zri_message_t 承载（见 message/message.h）；传输层只搬运字节，
 * kind/status/route/request_id 由 core 解释。
 */

#include <zrpc/zrpc_types.h>
#include <zrpc/zrpc_metrics.h>
#include <zrpc/zrpc_transport_provider.h>

#include <ztk/ztk.h>

#include <stdint.h>

#include "caps.h"
#include "message.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zri_transport zri_transport_t;
typedef struct zrpc_node zrpc_node_t;

typedef struct zri_transport_ops {
    /* 绑定/监听，成功后 t->port 为实际端口。 */
    int (*start)(zri_transport_t *t);
    void (*stop)(zri_transport_t *t);

    /* 发起发送：目标由 message->peer_ip / message->peer_port 指定。 */
    int (*send)(zri_transport_t *t, const zri_message_t *message);

    /* 回复：优先使用 message->peer_token，缺失时回退到 peer_ip:peer_port。 */
    int (*reply)(zri_transport_t *t, const zri_message_t *message);

    /* 查询某对端的链路统计（可选）：1 = 找到，0 = 无此对端，负数 = 出错。 */
    int (*peer_stats)(zri_transport_t *t, const char *ip, uint16_t port,
                      zrpc_peer_stats_t *out);
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

/* 传输层把收到的完整消息上抛给 core（实现在 dispatch.c）。 */
void zri_node_on_message(zrpc_node_t *node, zri_transport_t *transport,
                         const zri_message_t *message);

/* 具体驱动工厂（返回已初始化 ops/kind 的对象，未 start）。 */
zri_transport_t *zri_udp_transport_create(void);
zri_transport_t *zri_tcp_transport_create(void);

/* 便捷包装 */
int zri_transport_start(zri_transport_t *t);
void zri_transport_stop(zri_transport_t *t);
void zri_transport_destroy(zri_transport_t *t);
int zri_transport_send(zri_transport_t *t, const zri_message_t *message);
int zri_transport_reply(zri_transport_t *t, const zri_message_t *message);
const zrpc_transport_provider_t *zri_transport_provider_find(const char *scheme);

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_TRANSPORT_H */
