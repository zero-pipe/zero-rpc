#ifndef NB_TRANSPORT_INTERNAL_H
#define NB_TRANSPORT_INTERNAL_H

/*
 * 传输抽象：node 只依赖该接口，不感知 UDP/TCP 差异。
 *
 * 一个传输负责：绑定本地端口、把一条完整消息发给某个对端、
 * 把收到的完整消息上抛给 core（nb_node_on_message），以及自身的周期性驱动。
 *
 * 消息统一用 nb_message_t 承载（见 message/message.h）；传输层只搬运字节，
 * kind/status/route/request_id 由 core 解释。
 */

#include <types.h>
#include <metrics.h>
#include <transport_provider.h>

#include <ntk.h>

#include <stdint.h>

#include "caps.h"
#include "message.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nb_transport nb_transport_t;
typedef struct nb_node nb_node_t;

typedef struct nb_transport_ops {
    /* 绑定/监听，成功后 t->port 为实际端口。 */
    int (*start)(nb_transport_t *t);
    void (*stop)(nb_transport_t *t);

    /* 发起发送：目标由 message->peer_ip / message->peer_port 指定。 */
    int (*send)(nb_transport_t *t, const nb_message_t *message);

    /* 回复：优先使用 message->peer_token，缺失时回退到 peer_ip:peer_port。 */
    int (*reply)(nb_transport_t *t, const nb_message_t *message);

    /* 查询某对端的链路统计（可选）：1 = 找到，0 = 无此对端，负数 = 出错。 */
    int (*peer_stats)(nb_transport_t *t, const char *ip, uint16_t port, nb_peer_stats_t *out);
} nb_transport_ops_t;

struct nb_transport {
    const nb_transport_ops_t *ops;
    nb_node_t *node;
    nb_transport_kind_t kind;
    char scheme[NB_SCHEME_MAX];
    char bind_host[NB_ENDPOINT_MAX];
    uint16_t port;
    const nb_transport_provider_t *provider;
    nb_transport_instance_t *provider_instance;

    /*
     * 运行期配置快照：由 node 在建立传输时填充。传输层只读这些字段，
     * 不穿透 struct nb_node 内部，从而与 core 解耦。
     */
    uint16_t frag_bytes;
    uint32_t max_msg_bytes;
    uint32_t bp_high_water;
    uint8_t drop_percent;
    uint32_t rtcp_interval_ms;
    int rtcp_disabled;
    unsigned io_threads;
    ntk_poller_pool *pool;
    ntk_poller *poller;
    nb_metrics_t *metrics;
};

/* 传输层把收到的完整消息上抛给 core（实现在 dispatch.c）。 */
void nb_node_on_message(nb_node_t *node, nb_transport_t *transport, const nb_message_t *message);

/* 具体驱动工厂（返回已初始化 ops/kind 的对象，未 start）。 */
nb_transport_t *nb_udp_transport_create(void);
nb_transport_t *nb_tcp_transport_create(void);

/* 便捷包装 */
int nb_transport_start(nb_transport_t *t);
void nb_transport_stop(nb_transport_t *t);
void nb_transport_destroy(nb_transport_t *t);
int nb_transport_send(nb_transport_t *t, const nb_message_t *message);
int nb_transport_reply(nb_transport_t *t, const nb_message_t *message);
int nb_transport_subscribe(nb_transport_t *t, const char *service, const char *method);
void nb_transport_unsubscribe(nb_transport_t *t, const char *service, const char *method);
const nb_transport_provider_t *nb_transport_provider_find(const char *scheme);

#ifdef __cplusplus
}
#endif

#endif /* NB_TRANSPORT_INTERNAL_H */
