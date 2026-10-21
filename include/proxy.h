#ifndef PROXY_H
#define PROXY_H

/*
 * 客户端代理：call_sync / call_async / call_event 三种调用形态。
 *
 * 一个 node 可以同时承载 service（服务端）与 proxy（客户端），也可以只有其一。
 * 发现/路由在框架内部完成；proxy 只负责把 payload 送出去并把响应回调回来。
 *
 * 线程约定：call_async / call_event 可在任意线程调用（内部投递到 poller）；
 * call_sync 会阻塞等待，禁止在 poller 回调内使用。
 */

#include <types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct proxy_options {
    uint8_t payload_encoding;         /* 写入请求的默认编码标识，0 = 裸字节 */
    uint32_t prefer_stream_threshold; /* 0=disabled; larger payloads prefer TCP */
} proxy_options_t;

/*
 * 客户端拦截器（对齐 gRPC unary client interceptor 的最小形态）。
 * before 在调用线程执行，可修改 metadata / timeout；非 OK 返回值直接短路。
 * after 在 poller 线程、用户回调之前执行，可观测最终状态与响应 metadata。
 */
typedef struct proxy_interceptor {
    void *ctx;
    int (*before)(void *ctx,
                  const char *service,
                  const char *method,
                  metadata_t *metadata,
                  uint64_t *timeout_ms);
    void (*after)(void *ctx, int status, const response_t *resp);
} proxy_interceptor_t;

/* 安装/清除客户端拦截器；interceptor 为 NULL 时清除。 */
int proxy_set_interceptor(proxy_t *proxy, const proxy_interceptor_t *interceptor);

/* ---- 生命周期 ---- */
int proxy_create(node_t *node, const proxy_options_t *options, proxy_t **out);
void proxy_destroy(proxy_t *proxy);

/* ---- 目标端点 ---- */
/*
 * 直接指定目标数据端点（ip:port + 传输种类），调用跳过服务发现。
 * 适合已知对端地址的高性能场景。
 */
int proxy_set_endpoint(proxy_t *proxy, const char *ip, uint16_t port, transport_kind_t transport);
/* 直接指定目标端点 URI（如 tcp://127.0.0.1:8080、dds://...），跳过服务发现。 */
int proxy_set_endpoint_uri(proxy_t *proxy, const char *endpoint);

/* ---- Unary 调用 ---- */
/*
 * 异步请求：发现并发送后立即返回；响应或超时通过 cb 回调（poller 线程）。
 * timeout_ms：0 表示使用默认（DEFAULT_TIMEOUT_MS）。
 * out_request_id：非空时回填关联 ID，可用于 proxy_cancel。
 *
 * payload 所有权：框架在返回前复制 payload（包括跨线程调用）；调用方可在
 * 本函数返回后立即释放 payload->data，无需等回调。框架不会接管或释放它。
 */
int proxy_call_async(proxy_t *proxy,
                     const char *service,
                     const char *method,
                     const payload_t *payload,
                     response_fn cb,
                     void *user,
                     uint64_t timeout_ms,
                     uint64_t *out_request_id);
/* 带 metadata 的异步请求。 */
int proxy_call_async_ex(proxy_t *proxy,
                        const char *service,
                        const char *method,
                        const payload_t *payload,
                        const metadata_t *metadata,
                        response_fn cb,
                        void *user,
                        uint64_t timeout_ms,
                        uint64_t *out_request_id);

/*
 * 单向事件：只发送、不等待响应（EVENT）。发送在 poller 线程完成。
 * out_request_id 可空；非空时回填本次事件的关联 ID。
 * 与 proxy_call_async 相同，payload 在返回前已复制，调用方可立即释放。
 */
int proxy_call_event(proxy_t *proxy,
                     const char *service,
                     const char *method,
                     const payload_t *payload,
                     uint64_t *out_request_id);
int proxy_call_event_ex(proxy_t *proxy,
                        const char *service,
                        const char *method,
                        const payload_t *payload,
                        const metadata_t *metadata,
                        uint64_t *out_request_id);

/*
 * 同步请求：阻塞直到响应、错误或超时。成功时 response->payload 由内部缓冲持有，
 * 用完调用 response_release()。禁止在 poller 线程回调里调用。
 */
int proxy_call_sync(proxy_t *proxy,
                    const char *service,
                    const char *method,
                    const payload_t *request,
                    response_t *response,
                    uint64_t timeout_ms,
                    uint64_t *out_request_id);
int proxy_call_sync_ex(proxy_t *proxy,
                       const char *service,
                       const char *method,
                       const payload_t *request,
                       const metadata_t *metadata,
                       response_t *response,
                       uint64_t timeout_ms,
                       uint64_t *out_request_id);

/* 释放 proxy_call_sync 填出的响应缓冲并清零。 */
void response_release(response_t *response);

/* ---- 调用控制 ---- */
/* 取消未完成的调用；若已回调则返回 ERR_NOTFOUND。 */
int proxy_cancel(proxy_t *proxy, uint64_t request_id);

/* ---- 流式调用 ---- */
/* 客户端请求为普通 unary，响应按 chunk 回调，直到 LAST。 */
int proxy_call_stream(proxy_t *proxy,
                      const char *service,
                      const char *method,
                      const payload_t *request,
                      response_chunk_fn on_chunk,
                      stream_done_fn on_done,
                      void *user,
                      uint64_t timeout_ms,
                      uint64_t *out_request_id);

/* 打开客户端流。 */
int proxy_stream_open(proxy_t *proxy,
                      const char *service,
                      const char *method,
                      response_fn cb,
                      void *user,
                      uint64_t timeout_ms,
                      stream_t **out_stream);
/* 双向流：请求分片发送，同时接收响应分片。 */
int proxy_stream_open_bidi(proxy_t *proxy,
                           const char *service,
                           const char *method,
                           response_chunk_fn on_chunk,
                           stream_done_fn on_done,
                           void *user,
                           uint64_t timeout_ms,
                           stream_t **out_stream);

/*
 * 流式调用：分片发送请求，最后一个 chunk 必须带 STREAM_LAST。
 * chunk 在 send 返回后即可释放；框架已复制必要数据。
 */
int proxy_stream_send(stream_t *stream, const chunk_t *chunk);
int proxy_stream_cancel(stream_t *stream);
int proxy_stream_destroy(stream_t *stream);

#ifdef __cplusplus
}
#endif

#endif /* PROXY_H */
