#ifndef ZRPC_PROXY_H
#define ZRPC_PROXY_H

/*
 * 客户端代理：发起异步调用。
 *
 * 发现/路由在框架内部完成；代理只负责把 payload 送出去并把响应回调回来。
 */

#include <zrpc/zrpc_types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zrpc_proxy_options {
    uint8_t payload_encoding; /* 写入请求的默认编码标识，0 = 裸字节 */
    uint32_t prefer_stream_threshold; /* 0=disabled; larger payloads prefer TCP */
} zrpc_proxy_options_t;

/* 客户端响应回调。status：ZRPC_OK 或错误码；resp 仅在回调内有效。 */
typedef void (*zrpc_response_fn)(int status, const zrpc_response_t *resp, void *user);
typedef void (*zrpc_response_chunk_fn)(int status, const zrpc_chunk_t *chunk, void *user);
typedef void (*zrpc_stream_done_fn)(int status, void *user);

int zrpc_proxy_create(zrpc_node_t *node, const zrpc_proxy_options_t *options, zrpc_proxy_t **out);
void zrpc_proxy_destroy(zrpc_proxy_t *proxy);

/*
 * 发起异步调用。发现并发送后立即返回；响应或超时通过 cb 回调。
 * timeout_ms：0 表示使用默认（ZRPC_DEFAULT_TIMEOUT_MS）。
 * out_request_id：非空时回填关联 ID，可用于 zrpc_proxy_cancel。
 */
int zrpc_proxy_call(zrpc_proxy_t *proxy, const char *service, const char *method,
                    const zrpc_payload_t *payload, zrpc_response_fn cb, void *user,
                    uint64_t timeout_ms, uint64_t *out_request_id);

/* 请求为普通 unary，响应按 chunk 回调，直到 LAST。 */
int zrpc_proxy_call_stream(zrpc_proxy_t *proxy, const char *service, const char *method,
                           const zrpc_payload_t *request, zrpc_response_chunk_fn on_chunk,
                           zrpc_stream_done_fn on_done, void *user, uint64_t timeout_ms,
                           uint64_t *out_request_id);

/* 取消未完成的调用；若已回调则返回 ZRPC_ERR_NOTFOUND。 */
int zrpc_proxy_cancel(zrpc_proxy_t *proxy, uint64_t request_id);

/*
 * 直接指定目标数据端点（ip:port + 传输种类），调用跳过服务发现。
 * 适合已知对端地址的高性能场景。
 */
int zrpc_proxy_set_endpoint(zrpc_proxy_t *proxy, const char *ip, uint16_t port,
                            zrpc_transport_kind_t transport);
/* 直接指定目标端点 URI（如 tcp://127.0.0.1:8080、dds://...），跳过服务发现。 */
int zrpc_proxy_set_endpoint_uri(zrpc_proxy_t *proxy, const char *endpoint);

/*
 * 流式调用：分片发送请求。最后一个 chunk 必须带 ZRPC_STREAM_LAST。
 * chunk 在 send 返回后即可释放；框架已复制必要数据。
 */
int zrpc_proxy_stream_open(zrpc_proxy_t *proxy, const char *service, const char *method,
                           zrpc_response_fn cb, void *user, uint64_t timeout_ms,
                           zrpc_stream_t **out_stream);
int zrpc_proxy_stream_send(zrpc_stream_t *stream, const zrpc_chunk_t *chunk);
int zrpc_proxy_stream_cancel(zrpc_stream_t *stream);
int zrpc_proxy_stream_destroy(zrpc_stream_t *stream);

/* 双向流：请求分片发送，同时接收响应分片。 */
int zrpc_proxy_stream_open_bidi(zrpc_proxy_t *proxy, const char *service, const char *method,
                                zrpc_response_chunk_fn on_chunk, zrpc_stream_done_fn on_done,
                                void *user, uint64_t timeout_ms, zrpc_stream_t **out_stream);

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_PROXY_H */
