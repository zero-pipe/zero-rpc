#ifndef PROXY_INTERNAL_H
#define PROXY_INTERNAL_H

/* 客户端内部结构：proxy、stub、流式会话与调用上下文。 */

#include <proxy.h>
#include <stub.h>

#include <stddef.h>
#include <stdint.h>

#include "caps.h"
#include "pending.h"

struct nb_proxy {
    nb_node_t *node;
    uint8_t payload_encoding;
    uint32_t prefer_stream_threshold;
    nb_stub_t *stub;
    int has_endpoint;
    char endpoint_ip[NB_ENDPOINT_MAX];
    char endpoint_scheme[NB_SCHEME_MAX];
    uint16_t endpoint_port;
    nb_transport_kind_t endpoint_transport;
    int has_interceptor;
    nb_proxy_interceptor_t interceptor;
};

/* Stub：node 之上的传输无关调用原语；proxy 在它之上注入路由。 */
struct nb_stub {
    nb_node_t *node;
    uint8_t payload_encoding;
    stub_send_fn send;
    void *user_data;
};

struct nb_stream {
    nb_proxy_t *proxy;
    nb_stub_t *stub; /* open 后只依赖 stub 指针做 owner 标记，不再解引用 proxy */
    nb_node_t *node;
    uint64_t request_id;
    uint32_t stream_id;
    char service[NB_SERVICE_NAME_MAX];
    char method[NB_METHOD_NAME_MAX];
    char ip[NB_ENDPOINT_MAX];
    char scheme[NB_SCHEME_MAX];
    uint16_t port;
    nb_transport_kind_t transport;
    response_fn cb;
    response_chunk_fn chunk_cb;
    stream_done_fn done_cb;
    void *user;
    int bidi;
    uint64_t timeout_ms;
    uint64_t next_offset;
    int opened;
    int sent_any;
    int send_closed;
    int closed;
};

int nb_proxy_resolve(nb_proxy_t *proxy,
                      const char *service,
                      char *ip_out,
                      size_t ip_cap,
                      uint16_t *port_out,
                      nb_transport_kind_t *transport_out,
                      char *scheme_out,
                      size_t scheme_cap,
                      int prefer_stream);

/* stub.c：带流式回调的内部调用入口；供 proxy 复用。 */
int nb_stub_call_ex(nb_stub_t *stub,
                     const char *service,
                     const char *method,
                     const nb_payload_t *payload,
                     response_fn cb,
                     response_chunk_fn chunk_cb,
                     stream_done_fn done_cb,
                     int streaming,
                     void *user,
                     uint64_t timeout_ms,
                     uint64_t *out_request_id,
                     const nb_metadata_t *metadata);

/* 取消该 stub 名下所有未完成调用（proxy/stub 销毁前调用，避免回调 UAF）。 */
int nb_stub_cancel_all(nb_stub_t *stub, int status);

#endif /* PROXY_INTERNAL_H */
