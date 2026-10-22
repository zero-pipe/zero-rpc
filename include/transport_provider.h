#ifndef TRANSPORT_PROVIDER_H
#define TRANSPORT_PROVIDER_H

#include <types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct transport_provider transport_provider_t;
typedef struct transport_instance transport_instance_t;

/* ---- 消息与回调 ---- */
/*
 * Provider 消息视图与框架内部 zri_message_t 的语义保持一致（寻址/回程 token
 * 由 send/reply/receive 的独立参数承载）。新增字段必须同步在
 * transport_factory.c 的双向适配中透传，避免自定义传输丢语义。
 */
typedef struct transport_message {
    uint8_t kind;
    uint8_t flags; /* MSG_FLAG_*；随包透传 */
    int status; /* RESPONSE：应用状态码；带 MSG_FLAG_FRAMEWORK_STATUS 时为框架错误码 */
    uint32_t request_id;
    uint32_t timeout_ms; /* 请求剩余超时毫秒；0 = 无 deadline */
    const void *metadata; /* metadata TLV 字节；可为 NULL */
    uint16_t metadata_len;
    const char *status_msg; /* 状态消息；可为 NULL */
    uint16_t status_msg_len;
    int has_stream; /* 非 0 时 stream 有效 */
    stream_meta_t stream;
    const char *route;
    payload_t payload;
} transport_message_t;

typedef void (*transport_receive_fn)(void *receiver,
                                     void *reply_token,
                                     const char *peer_host,
                                     uint16_t peer_port,
                                     const transport_message_t *message);

/* ---- Provider 生命周期与操作 ---- */
typedef struct transport_provider_options {
    const char *scheme;
    const char *endpoint;
    void *receiver;
    transport_receive_fn on_receive;
} transport_provider_options_t;

/* send() returning OK means the provider accepted the message for delivery. */
typedef struct transport_provider_ops {
    int (*create)(const transport_provider_options_t *options, transport_instance_t **out);
    int (*start)(transport_instance_t *instance);
    void (*stop)(transport_instance_t *instance);
    void (*destroy)(transport_instance_t *instance);
    int (*send)(transport_instance_t *instance,
                const char *host,
                uint16_t port,
                const transport_message_t *message);
    int (*reply)(transport_instance_t *instance,
                 void *reply_token,
                 const char *host,
                 uint16_t port,
                 const transport_message_t *message);
} transport_provider_ops_t;

struct transport_provider {
    const char *scheme;
    const transport_provider_ops_t *ops;
};

/* ---- 注册 ---- */
/* Register before creating nodes. Provider descriptors and ops must have static lifetime. */
int transport_provider_register(const transport_provider_t *provider);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_PROVIDER_H */
