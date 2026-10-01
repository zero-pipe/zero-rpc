#ifndef ZRPC_SERVICE_H
#define ZRPC_SERVICE_H

/*
 * 服务端：注册 service / method 与处理请求。
 *
 * handler 收到请求视图后，必须在未来某个时刻调用 zrpc_reply()（可同步可异步）。
 * 请求视图（含 payload）仅在 handler 内有效，异步回复前请自行拷贝。
 */

#include <zrpc/zrpc_types.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zrpc_request {
    const char *service;
    const char *method;
    zrpc_payload_t payload; /* 对框架透明的业务字节 */
    const char *peer_ip;
    uint16_t peer_port;
} zrpc_request_t;

typedef struct zrpc_response {
    int status;             /* 应用定义，0 建议表示成功 */
    zrpc_payload_t payload;
} zrpc_response_t;

typedef void (*zrpc_handler_fn)(zrpc_call_t *call, const zrpc_request_t *req, void *user);

/*
 * 流式请求回调。框架按到达顺序逐个 chunk 调用；chunk 仅在本次回调内有效。
 * flags 含 ZRPC_STREAM_FIRST / ZRPC_STREAM_LAST。
 */
typedef void (*zrpc_chunk_handler_fn)(zrpc_call_t *call, const zrpc_request_t *req,
                                      const zrpc_chunk_t *chunk, void *user);

/* 一次性、异步回复。status 由应用定义。 */
void zrpc_reply(zrpc_call_t *call, int status, const zrpc_payload_t *payload);
/* 便捷版本：直接回复裸字节（encoding = 0）。 */
void zrpc_reply_bytes(zrpc_call_t *call, int status, const void *data, size_t len);
/* 分多次发送 response chunk；最后一个 chunk 必须带 ZRPC_STREAM_LAST。 */
int zrpc_reply_stream(zrpc_call_t *call, int status, const zrpc_chunk_t *chunk);

int zrpc_service_create(zrpc_node_t *node, const char *service_name, zrpc_service_t **out);
void zrpc_service_destroy(zrpc_service_t *svc);
int zrpc_service_add_method(zrpc_service_t *svc, const char *method, zrpc_handler_fn fn, void *user);
int zrpc_service_add_stream_method(zrpc_service_t *svc, const char *method,
                                   zrpc_chunk_handler_fn fn, void *user);

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_SERVICE_H */
