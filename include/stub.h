#ifndef NB_STUB_H
#define NB_STUB_H

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
typedef struct nb_stub nb_stub_t;

/* 一次调用的目标，由 send 回调解析后回填（用于出错重传）。 */
typedef struct nb_stub_target {
    char ip[NB_ENDPOINT_MAX];
    char scheme[NB_SCHEME_MAX];
    uint16_t port;
    nb_transport_kind_t transport;
} nb_stub_target_t;

typedef enum nb_stub_kind {
    NB_STUB_REQUEST = 0, /* 需要响应 */
    NB_STUB_EVENT = 1    /* 单向，不等响应 */
} nb_stub_kind_t;

/*
 * 底层单条消息发送钩子。实现者负责解析目标、回填 target 并调用真实发送。
 * payload 仅在本次调用期间有效。返回 NB_OK 或错误码。
 */
typedef int (*stub_send_fn)(nb_stub_t *stub,
                            const char *service,
                            const char *method,
                            const nb_payload_t *payload,
                            nb_stub_kind_t kind,
                            uint64_t request_id,
                            uint64_t timeout_ms,
                            const void *metadata,
                            uint16_t metadata_len,
                            nb_stub_target_t *target,
                            void *user_data);

typedef struct nb_stub_options {
    uint8_t payload_encoding; /* 写入请求的默认编码标识，0 = 裸字节 */
    stub_send_fn send;        /* 可空：空则使用节点默认路由 */
    void *user_data;          /* 传给 send 的用户数据，stub 不负责释放 */
} nb_stub_options_t;

/* ---- 生命周期 ---- */
int nb_stub_create(nb_node_t *node, const nb_stub_options_t *options, nb_stub_t **out);
void nb_stub_destroy(nb_stub_t *stub);

/* 借用底层节点。 */
nb_node_t *nb_stub_node(const nb_stub_t *stub);

/* ---- 调用 ---- */
/* 单向发送（EVENT）。不等待响应；发送在节点 poller 线程上完成。 */
int nb_stub_call_event(nb_stub_t *stub,
                    const char *service,
                    const char *method,
                    const nb_payload_t *payload,
                    uint64_t *out_request_id);
int nb_stub_call_event_ex(nb_stub_t *stub,
                       const char *service,
                       const char *method,
                       const nb_payload_t *payload,
                       const nb_metadata_t *metadata,
                       uint64_t *out_request_id);

/*
 * 异步请求。发现并发送后立即返回；响应或超时通过 cb 回调（在 poller 线程）。
 * timeout_ms 为 0 时使用 NB_DEFAULT_TIMEOUT_MS。
 * payload 在返回前已复制（跨线程同样），调用方可立即释放自己的缓冲区。
 */
int nb_stub_call_async(nb_stub_t *stub,
                    const char *service,
                    const char *method,
                    const nb_payload_t *payload,
                    response_fn cb,
                    void *user,
                    uint64_t timeout_ms,
                    uint64_t *out_request_id);
/* 带 metadata 的异步请求；metadata 为 NULL 时等价于 nb_stub_call_async。 */
int nb_stub_call_async_ex(nb_stub_t *stub,
                       const char *service,
                       const char *method,
                       const nb_payload_t *payload,
                       const nb_metadata_t *metadata,
                       response_fn cb,
                       void *user,
                       uint64_t timeout_ms,
                       uint64_t *out_request_id);

#ifdef __cplusplus
}
#endif

#endif /* NB_STUB_H */
