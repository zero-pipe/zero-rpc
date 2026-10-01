#ifndef ZRPC_TRANSPORT_PROVIDER_H
#define ZRPC_TRANSPORT_PROVIDER_H

#include <zrpc/zrpc_types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zrpc_transport_provider zrpc_transport_provider_t;
typedef struct zrpc_transport_instance zrpc_transport_instance_t;

typedef struct zrpc_transport_message {
    uint8_t kind;
    uint32_t request_id;
    zrpc_stream_meta_t stream;
    const char *route;
    zrpc_payload_t payload;
} zrpc_transport_message_t;

typedef void (*zrpc_transport_receive_fn)(void *receiver, void *reply_token, const char *peer_host,
                                          uint16_t peer_port,
                                          const zrpc_transport_message_t *message);

typedef struct zrpc_transport_provider_options {
    const char *scheme;
    const char *endpoint;
    void *receiver;
    zrpc_transport_receive_fn on_receive;
} zrpc_transport_provider_options_t;

/* send() returning ZRPC_OK means the provider accepted the message for delivery. */
typedef struct zrpc_transport_provider_ops {
    int (*create)(const zrpc_transport_provider_options_t *options,
                  zrpc_transport_instance_t **out);
    int (*start)(zrpc_transport_instance_t *instance);
    void (*stop)(zrpc_transport_instance_t *instance);
    void (*destroy)(zrpc_transport_instance_t *instance);
    int (*send)(zrpc_transport_instance_t *instance, const char *host, uint16_t port,
                const zrpc_transport_message_t *message);
    int (*reply)(zrpc_transport_instance_t *instance, void *reply_token, const char *host,
                 uint16_t port, const zrpc_transport_message_t *message);
} zrpc_transport_provider_ops_t;

struct zrpc_transport_provider {
    const char *scheme;
    const zrpc_transport_provider_ops_t *ops;
};

/* Register before creating nodes. Provider descriptors and ops must have static lifetime. */
int zrpc_transport_provider_register(const zrpc_transport_provider_t *provider);

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_TRANSPORT_PROVIDER_H */
