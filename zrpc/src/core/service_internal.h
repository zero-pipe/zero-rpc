#ifndef ZRPC_SERVICE_INTERNAL_H
#define ZRPC_SERVICE_INTERNAL_H

/* 服务端内部结构：方法表、服务对象、调用上下文、流接收状态。 */

#include <zrpc/zrpc_service.h>

#include <stddef.h>
#include <stdint.h>

#include "caps.h"
#include "transport.h"

typedef struct zri_method {
    struct zri_method *next;
    char name[ZRPC_METHOD_NAME_MAX];
    zrpc_handler_fn fn;
    zrpc_chunk_handler_fn stream_fn;
    void *user;
    int owns_user; /* user 由框架分配（如 sync 桥接结构），销毁时释放 */
} zri_method_t;

struct zrpc_service {
    struct zrpc_service *next;
    zrpc_node_t *node;
    char name[ZRPC_SERVICE_NAME_MAX];
    zri_method_t *methods;
};

struct zrpc_call {
    zrpc_node_t *node;
    zri_transport_t *transport;
    zri_peer_token_t token;
    char peer_ip[ZRPC_ENDPOINT_MAX];
    uint16_t peer_port;
    uint32_t msg_id;
    int replied;
    int one_way; /* EVENT：handler 可复用，但不产生响应 */
    int stream_owned;
};

typedef struct zri_recv_stream {
    int used;
    uint32_t request_id;
    char peer_ip[ZRPC_ENDPOINT_MAX];
    uint16_t peer_port;
    zrpc_call_t *call;
    struct zri_method *method;
    char service[ZRPC_SERVICE_NAME_MAX];
    char method_name[ZRPC_METHOD_NAME_MAX];
    uint64_t last_ms;
    uint64_t bytes;
} zri_recv_stream_t;

void zri_service_handle_request(zrpc_node_t *node, zri_transport_t *transport,
                                const zri_message_t *message);
void zri_service_handle_stream_chunk(zrpc_node_t *node, zri_transport_t *transport,
                                     const zri_message_t *message);
void zri_service_expire_streams(zrpc_node_t *node, uint64_t now_ms);

/* reply.c：错误状态回包（路由/分配失败时直接发送，无 call 上下文）。 */
void zri_service_send_status(zri_transport_t *transport, const zri_message_t *request, int status);

#endif /* ZRPC_SERVICE_INTERNAL_H */
