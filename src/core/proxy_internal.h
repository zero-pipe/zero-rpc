#ifndef PROXY_INTERNAL_H
#define PROXY_INTERNAL_H

/* 客户端内部结构：proxy、stub、流式会话与调用上下文。 */

#include <proxy.h>
#include <stub.h>

#include <stddef.h>
#include <stdint.h>

#include "caps.h"
#include "pending.h"

struct proxy {
    node_t *node;
    uint8_t payload_encoding;
    uint32_t prefer_stream_threshold;
    stub_t *stub;
    int has_endpoint;
    char endpoint_ip[ENDPOINT_MAX];
    char endpoint_scheme[SCHEME_MAX];
    uint16_t endpoint_port;
    transport_kind_t endpoint_transport;
    int has_interceptor;
    proxy_interceptor_t interceptor;
};

/* Stub：node 之上的传输无关调用原语；proxy 在它之上注入路由。 */
struct stub {
    node_t *node;
    uint8_t payload_encoding;
    stub_send_fn send;
    void *user_data;
};

struct stream {
    proxy_t *proxy;
    stub_t *stub; /* open 后只依赖 stub 指针做 owner 标记，不再解引用 proxy */
    node_t *node;
    uint64_t request_id;
    uint32_t stream_id;
    char service[SERVICE_NAME_MAX];
    char method[METHOD_NAME_MAX];
    char ip[ENDPOINT_MAX];
    char scheme[SCHEME_MAX];
    uint16_t port;
    transport_kind_t transport;
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

int zri_proxy_resolve(proxy_t *proxy,
                      const char *service,
                      char *ip_out,
                      size_t ip_cap,
                      uint16_t *port_out,
                      transport_kind_t *transport_out,
                      char *scheme_out,
                      size_t scheme_cap,
                      int prefer_stream);

/* stub.c：带流式回调的内部调用入口；供 proxy 复用。 */
int zri_stub_call_ex(stub_t *stub,
                     const char *service,
                     const char *method,
                     const payload_t *payload,
                     response_fn cb,
                     response_chunk_fn chunk_cb,
                     stream_done_fn done_cb,
                     int streaming,
                     void *user,
                     uint64_t timeout_ms,
                     uint64_t *out_request_id,
                     const metadata_t *metadata);

/* 取消该 stub 名下所有未完成调用（proxy/stub 销毁前调用，避免回调 UAF）。 */
int zri_stub_cancel_all(stub_t *stub, int status);

#endif /* PROXY_INTERNAL_H */
