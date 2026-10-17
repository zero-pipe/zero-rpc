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
typedef struct transport_message {
    uint8_t kind;
    int status; /* 应用状态码；仅 RESPONSE 有意义 */
    uint32_t request_id;
    stream_meta_t stream;
    const char *route;
    payload_t payload;
} transport_message_t;

typedef void (*transport_receive_fn)(void *receiver, void *reply_token, const char *peer_host,
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
    int (*create)(const transport_provider_options_t *options,
                  transport_instance_t **out);
    int (*start)(transport_instance_t *instance);
    void (*stop)(transport_instance_t *instance);
    void (*destroy)(transport_instance_t *instance);
    int (*send)(transport_instance_t *instance, const char *host, uint16_t port,
                const transport_message_t *message);
    int (*reply)(transport_instance_t *instance, void *reply_token, const char *host,
                 uint16_t port, const transport_message_t *message);
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
