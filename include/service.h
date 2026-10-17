#ifndef SERVICE_H
#define SERVICE_H

/*
 * 服务端：注册 service / method 与处理请求。
 *
 * handler 收到请求视图后，必须在未来某个时刻调用 reply()（可同步可异步）。
 * 请求视图（含 payload）仅在 handler 内有效，异步回复前请自行拷贝。
 */

#include <types.h>
#include <message_view.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*handler_fn)(call_t *call, const request_t *req, void *user);

/*
 * 同步风格 handler：返回值即应用状态码；把响应写进 resp view，框架在返回后
 * 自动回复（回复本身仍是异步发送）。resp 拥有自己的缓冲，调用方无需管理。
 */
typedef int (*sync_handler_fn)(const request_t *req, message_view_t *resp,
                                    void *user);

/*
 * 流式请求回调。框架按到达顺序逐个 chunk 调用；chunk 仅在本次回调内有效。
 * flags 含 STREAM_FIRST / STREAM_LAST。
 */
typedef void (*chunk_handler_fn)(call_t *call, const request_t *req,
                                      const chunk_t *chunk, void *user);

/* ---- 服务生命周期 ---- */
int service_create(node_t *node, const char *service_name, service_t **out);
void service_destroy(service_t *svc);

/* ---- 方法注册 ---- */
int service_add_method(service_t *svc, const char *method, handler_fn fn, void *user);
/* 注册同步风格方法（handler 返回后框架自动回复）。 */
int service_add_sync_method(service_t *svc, const char *method,
                                 sync_handler_fn fn, void *user);
int service_add_stream_method(service_t *svc, const char *method,
                                    chunk_handler_fn fn, void *user);

/* ---- 回复 ---- */
/* 一次性、异步回复。status 由应用定义；EVENT 请求上的回复会被静默忽略。 */
void reply(call_t *call, int status, const payload_t *payload);
/* 便捷版本：直接回复裸字节（encoding = 0）。 */
void reply_bytes(call_t *call, int status, const void *data, size_t len);
/* 分多次发送 response chunk；最后一个 chunk 必须带 STREAM_LAST。 */
int reply_stream(call_t *call, int status, const chunk_t *chunk);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_H */
