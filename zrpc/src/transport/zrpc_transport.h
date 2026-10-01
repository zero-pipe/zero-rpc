#ifndef ZRPC_TRANSPORT_H
#define ZRPC_TRANSPORT_H

/*
 * 传输抽象：node 只依赖该接口，不感知 UDP/TCP 差异。
 *
 * 一个传输负责：绑定本地端口、把一条完整 RPC 消息发给某个对端、
 * 把收到的完整消息上抛给 node（zrpc_node_on_message），以及自身的周期性驱动。
 *
 * payload 在传输层是纯字节；kind/msg_id/route 由上层（core）解释。
 */

#include <zrpc/zrpc_types.h>
#include <zrpc/zrpc_transport_provider.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zrpc_transport zrpc_transport_t;
typedef struct zrpc_node zrpc_node_t;

/* 收到的完整消息上抛给 node 时携带的回程标识（传输私有，仅用于 reply）。 */
typedef void *zrpc_peer_token_t;

typedef struct zrpc_transport_ops {
    /* 绑定/监听，成功后 t->port 为实际端口。 */
    int (*start)(zrpc_transport_t *t);
    void (*stop)(zrpc_transport_t *t);

    /* 发起（首次）发送：按 ip:port 找到/建立通道。 */
    int (*send)(zrpc_transport_t *t, const char *ip, uint16_t port, uint8_t kind, uint32_t msg_id,
                const char *route, const zrpc_payload_t *payload,
                const zrpc_stream_meta_t *stream);

    /* 回复：优先使用 inbound 回调给出的 peer_token（回程通道），token 为空时回退到 ip:port。 */
    int (*reply)(zrpc_transport_t *t, zrpc_peer_token_t token, const char *ip, uint16_t port,
                 uint8_t kind, uint32_t msg_id, const char *route, const zrpc_payload_t *payload,
                 const zrpc_stream_meta_t *stream);
} zrpc_transport_ops_t;

struct zrpc_transport {
    const zrpc_transport_ops_t *ops;
    zrpc_node_t *node;
    zrpc_transport_kind_t kind;
    char scheme[ZRPC_SCHEME_MAX];
    char bind_host[ZRPC_ENDPOINT_MAX];
    uint16_t port;
    const zrpc_transport_provider_t *provider;
    zrpc_transport_instance_t *provider_instance;
};

/* 具体驱动工厂（返回已初始化 ops/kind 的对象，未 start）。 */
zrpc_transport_t *zrpc_udp_transport_create(void);
zrpc_transport_t *zrpc_tcp_transport_create(void);

/* 便捷包装 */
int zrpc_transport_start(zrpc_transport_t *t);
void zrpc_transport_stop(zrpc_transport_t *t);
void zrpc_transport_destroy(zrpc_transport_t *t);
int zrpc_transport_send(zrpc_transport_t *t, const char *ip, uint16_t port, uint8_t kind,
                         uint32_t msg_id, const char *route, const zrpc_payload_t *payload,
                         const zrpc_stream_meta_t *stream);
int zrpc_transport_reply(zrpc_transport_t *t, zrpc_peer_token_t token, const char *ip,
                         uint16_t port, uint8_t kind, uint32_t msg_id, const char *route,
                         const zrpc_payload_t *payload, const zrpc_stream_meta_t *stream);
const zrpc_transport_provider_t *zrpc_transport_provider_find(const char *scheme);

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_TRANSPORT_H */
