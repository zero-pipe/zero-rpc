#ifndef NB_TRANSPORT_PROVIDER_H
#define NB_TRANSPORT_PROVIDER_H

#include <types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nb_transport_provider nb_transport_provider_t;
typedef struct nb_transport_instance nb_transport_instance_t;

/* ---- 消息与回调 ---- */
/*
 * Provider 消息视图与框架内部 nb_message_t 的语义保持一致（寻址/回程 token
 * 由 send/nb_reply/receive 的独立参数承载）。新增字段必须同步在
 * transport_factory.c 的双向适配中透传，避免自定义传输丢语义。
 */
typedef struct nb_transport_message {
    uint8_t kind;
    uint8_t flags; /* MSG_FLAG_*；随包透传 */
    int status; /* RESPONSE：应用状态码；带 NB_MSG_FLAG_FRAMEWORK_STATUS 时为框架错误码 */
    uint32_t request_id;
    uint32_t timeout_ms; /* 请求剩余超时毫秒；0 = 无 deadline */
    const void *metadata; /* metadata TLV 字节；可为 NULL */
    uint16_t metadata_len;
    const char *status_msg; /* 状态消息；可为 NULL */
    uint16_t status_msg_len;
    int has_stream; /* 非 0 时 stream 有效 */
    nb_stream_meta_t stream;
    const char *route;
    nb_payload_t payload;
} nb_transport_message_t;

typedef void (*transport_receive_fn)(void *receiver,
                                     void *reply_token,
                                     const char *peer_host,
                                     uint16_t peer_port,
                                     const nb_transport_message_t *message);

/* ---- Provider 生命周期与操作 ---- */
typedef struct nb_transport_provider_options {
    const char *scheme;
    const char *endpoint;
    void *receiver;
    transport_receive_fn on_receive;
    nb_intra_process_t intra_process;
} nb_transport_provider_options_t;

/* send() returning NB_OK means the provider accepted the message for delivery. */
typedef struct nb_transport_provider_ops {
    int (*create)(const nb_transport_provider_options_t *options, nb_transport_instance_t **out);
    int (*start)(nb_transport_instance_t *instance);
    void (*stop)(nb_transport_instance_t *instance);
    void (*destroy)(nb_transport_instance_t *instance);
    int (*send)(nb_transport_instance_t *instance,
                const char *host,
                uint16_t port,
                const nb_transport_message_t *message);
    int (*reply)(nb_transport_instance_t *instance,
                 void *reply_token,
                 const char *host,
                 uint16_t port,
                 const nb_transport_message_t *message);
    /*
     * Optional topic/service registration hooks for transports that need
     * explicit subscribers (for example DDS topics). Providers that do not
     * need them may leave these NULL.
     */
    int (*subscribe)(nb_transport_instance_t *instance,
                     const char *service,
                     const char *method);
    void (*unsubscribe)(nb_transport_instance_t *instance,
                        const char *service,
                        const char *method);
} nb_transport_provider_ops_t;

struct nb_transport_provider {
    const char *scheme;
    const nb_transport_provider_ops_t *ops;
};

/* ---- 注册 ---- */
/* Register before creating nodes. Provider descriptors and ops must have static lifetime. */
int nb_transport_provider_register(const nb_transport_provider_t *provider);

#ifdef __cplusplus
}
#endif

#endif /* NB_TRANSPORT_PROVIDER_H */
