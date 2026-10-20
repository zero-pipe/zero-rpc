#ifndef STUB_H
#define STUB_H

/*
 * Stub：传输无关的底层发送原语。
 *
 * 对齐 fastcall-c 的 fc_stub 分层：node 负责生命周期与传输，stub 负责把
 * service.method + payload 组装成一次调用并维护关联（request_id），
 * proxy 则在 stub 之上注入服务发现/路由（见 proxy.h）。
 *
 * 不注入 send 回调时，stub 使用节点默认路由（静态路由表 + 服务发现）。
 */

#include <types.h>
#include <endpoint.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 目标与发送钩子 ---- */
typedef struct stub stub_t;

/* 一次调用的目标，由 send 回调解析后回填（用于出错重传）。 */
typedef struct stub_target {
    char ip[ENDPOINT_MAX];
    char scheme[SCHEME_MAX];
    uint16_t port;
    transport_kind_t transport;
} stub_target_t;

typedef enum stub_kind {
    STUB_REQUEST = 0, /* 需要响应 */
    STUB_EVENT = 1    /* 单向，不等响应 */
} stub_kind_t;

/*
 * 底层单条消息发送钩子。实现者负责解析目标、回填 target 并调用真实发送。
 * payload 仅在本次调用期间有效。返回 OK 或错误码。
 */
typedef int (*stub_send_fn)(stub_t *stub, const char *service, const char *method,
                                 const payload_t *payload, stub_kind_t kind,
                                 uint64_t request_id, uint64_t timeout_ms,
                                 stub_target_t *target, void *user_data);

typedef struct stub_options {
    uint8_t payload_encoding; /* 写入请求的默认编码标识，0 = 裸字节 */
    stub_send_fn send;   /* 可空：空则使用节点默认路由 */
    void *user_data;          /* 传给 send 的用户数据，stub 不负责释放 */
} stub_options_t;

/* ---- 生命周期 ---- */
int stub_create(node_t *node, const stub_options_t *options, stub_t **out);
void stub_destroy(stub_t *stub);

/* 借用底层节点。 */
node_t *stub_node(const stub_t *stub);

/* ---- 调用 ---- */
/* 单向发送（EVENT）。不等待响应；发送在节点 poller 线程上完成。 */
int stub_call(stub_t *stub, const char *service, const char *method,
                   const payload_t *payload, uint64_t *out_request_id);

/*
 * 异步请求。发现并发送后立即返回；响应或超时通过 cb 回调（在 poller 线程）。
 * timeout_ms 为 0 时使用 DEFAULT_TIMEOUT_MS。
 * payload 在返回前已复制（跨线程同样），调用方可立即释放自己的缓冲区。
 */
int stub_call_async(stub_t *stub, const char *service, const char *method,
                         const payload_t *payload, response_fn cb, void *user,
                         uint64_t timeout_ms, uint64_t *out_request_id);

#ifdef __cplusplus
}
#endif

#endif /* STUB_H */
