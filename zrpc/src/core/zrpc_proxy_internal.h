#ifndef ZRPC_PROXY_INTERNAL_H
#define ZRPC_PROXY_INTERNAL_H

/* 客户端内部结构：proxy、stub、流式会话与调用上下文。 */

#include <zrpc/zrpc_proxy.h>
#include <zrpc/zrpc_stub.h>

#include <stddef.h>
#include <stdint.h>

#include "zrpc_caps.h"
#include "zrpc_pending.h"

struct zrpc_proxy {
    zrpc_node_t *node;
    uint8_t payload_encoding;
    uint32_t prefer_stream_threshold;
    zrpc_stub_t *stub;
    int has_endpoint;
    char endpoint_ip[ZRPC_ENDPOINT_MAX];
    char endpoint_scheme[ZRPC_SCHEME_MAX];
    uint16_t endpoint_port;
    zrpc_transport_kind_t endpoint_transport;
};

/* Stub：node 之上的传输无关调用原语；proxy 在它之上注入路由。 */
struct zrpc_stub {
    zrpc_node_t *node;
    uint8_t payload_encoding;
    zrpc_stub_send_fn send;
    void *user_data;
};

struct zrpc_stream {
    zrpc_proxy_t *proxy;
    zrpc_node_t *node;
    uint64_t request_id;
    uint32_t stream_id;
    char service[ZRPC_SERVICE_NAME_MAX];
    char method[ZRPC_METHOD_NAME_MAX];
    char ip[ZRPC_ENDPOINT_MAX];
    char scheme[ZRPC_SCHEME_MAX];
    uint16_t port;
    zrpc_transport_kind_t transport;
    zrpc_response_fn cb;
    zrpc_response_chunk_fn chunk_cb;
    zrpc_stream_done_fn done_cb;
    void *user;
    int bidi;
    uint64_t timeout_ms;
    uint64_t next_offset;
    int opened;
    int sent_any;
    int send_closed;
    int closed;
};

int zrpc_proxy_resolve(zrpc_proxy_t *proxy, const char *service, char *ip_out, size_t ip_cap,
                       uint16_t *port_out, zrpc_transport_kind_t *transport_out,
                       char *scheme_out, size_t scheme_cap, int prefer_stream);

/* stub.c：带流式回调的内部调用入口；供 proxy 复用。 */
int zrpc_stub_call_ex(zrpc_stub_t *stub, const char *service, const char *method,
                      const zrpc_payload_t *payload, zrpc_response_fn cb,
                      zrpc_response_chunk_fn chunk_cb, zrpc_stream_done_fn done_cb,
                      int streaming, void *user, uint64_t timeout_ms, uint64_t *out_request_id);

#endif /* ZRPC_PROXY_INTERNAL_H */
