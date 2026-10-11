#ifndef ZRPC_SYNC_H
#define ZRPC_SYNC_H

/*
 * 同步调用便捷层（可选）。
 *
 * zrpc 核心保持“仅异步”：所有发送与回调都在 poller 线程完成，本文件只是在
 * 异步 API 之上加了一层阻塞等待，方便脚本/简单调用场景。禁止在 poller 线程
 * 回调里调用这些函数（会阻塞 IO 线程）。
 *
 * 返回的 response.payload 由内部缓冲持有，用完调用 zrpc_response_release()。
 */

#include <zrpc/zrpc_types.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 阻塞发起一次调用并等待响应。timeout_ms 为 0 时使用 ZRPC_DEFAULT_TIMEOUT_MS。
 * 成功返回 ZRPC_OK 并把响应写入 *response；超时返回 ZRPC_ERR_TIMEOUT，其他错误
 * 返回对应错误码。out_request_id 可空。
 */
int zrpc_proxy_call_sync(zrpc_proxy_t *proxy, const char *service, const char *method,
                         const zrpc_payload_t *request, zrpc_response_t *response,
                         uint64_t timeout_ms, uint64_t *out_request_id);

/* 释放 zrpc_proxy_call_sync 填出的响应缓冲并清零。 */
void zrpc_response_release(zrpc_response_t *response);

#ifdef __cplusplus
}
#endif

#endif /* ZRPC_SYNC_H */
