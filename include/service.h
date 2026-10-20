#ifndef SERVICE_H
#define SERVICE_H

/*
 * 服务端：注册 service / method 与处理请求。
 *
 * handler 收到请求视图后，必须在未来某个时刻调用 reply()（可同步可异步）。
 * 请求视图（含 payload）仅在 handler 内有效，异步回复前请自行拷贝。
 *
 * 线程模型：调用者在注册方法时通过 service_method_options_t 选择执行模式：
 *   - SERVICE_EXEC_INLINE（默认）：在请求到达的 ntk poller 线程内联执行，
 *     禁止阻塞；阻塞会卡死整条 poller 上的所有连接。
 *   - SERVICE_EXEC_WORKER：在节点配置的 handler_threads 线程池上执行，适合
 *     IO、锁、sleep 等耗时/阻塞业务；回复自动回投原 poller 发送。
 *   - SERVICE_EXEC_EXECUTOR：在调用者提供的 service_executor_t 上执行，
 *     适合复用应用已有线程池/事件循环；生命周期由调用者负责。
 * 未配置 handler_threads 时 WORKER 退化为 INLINE 并告警；任务入池失败时回
 * 错误状态，不会回退到 poller 内联执行。流式方法目前只支持 INLINE。
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

/* ---- 方法执行模式 ---- */

typedef enum service_exec_mode {
    SERVICE_EXEC_INLINE = 0,   /* 请求到达的 poller 线程内联执行（默认） */
    SERVICE_EXEC_WORKER = 1,   /* 节点 handler 线程池执行，回复回投原 poller */
    SERVICE_EXEC_EXECUTOR = 2  /* 调用者提供的 service_executor_t */
} service_exec_mode_t;

/*
 * 自定义执行器，语义对齐 gRPC 的 executor：由调用者创建和拥有，框架只调用
 * submit，不负责销毁或排空。调用者必须保证 executor 活过 node，并在
 * node_destroy 前停止提交、等待所有已提交任务完成。
 */
typedef struct service_executor {
    void *ctx;
    /*
     * 在 executor 的线程上执行 fn(user)。返回 OK 表示已接管任务；返回错误
     * 表示未接管（框架会释放任务并回错误状态）。
     */
    int (*submit)(void *ctx, void (*fn)(void *user), void *user);
} service_executor_t;

typedef struct service_method_options {
    service_exec_mode_t exec; /* 默认 SERVICE_EXEC_INLINE */
    /* exec == SERVICE_EXEC_EXECUTOR 时必须非 NULL，且 submit 不能为空。 */
    const service_executor_t *executor;
} service_method_options_t;

#define SERVICE_METHOD_OPTIONS_INIT { SERVICE_EXEC_INLINE, NULL }

/* ---- 服务生命周期 ---- */
int service_create(node_t *node, const char *service_name, service_t **out);
void service_destroy(service_t *svc);

/* ---- 方法注册 ---- */
/*
 * 普通方法。options 为 NULL 时等价于 SERVICE_EXEC_INLINE。
 * WORKER 模式需 node_config_t.handler_threads > 0。
 */
int service_add_method(service_t *svc, const char *method, handler_fn fn, void *user);
int service_add_method_ex(service_t *svc, const char *method, handler_fn fn, void *user,
                          const service_method_options_t *options);
/* 注册同步风格方法（handler 返回后框架自动回复）。 */
int service_add_sync_method(service_t *svc, const char *method,
                                 sync_handler_fn fn, void *user);
int service_add_sync_method_ex(service_t *svc, const char *method,
                               sync_handler_fn fn, void *user,
                               const service_method_options_t *options);
/* 流式方法目前只支持 SERVICE_EXEC_INLINE；传 WORKER 返回 ERR_INVALID。 */
int service_add_stream_method(service_t *svc, const char *method,
                                    chunk_handler_fn fn, void *user);
int service_add_stream_method_ex(service_t *svc, const char *method,
                                 chunk_handler_fn fn, void *user,
                                 const service_method_options_t *options);

/*
 * 等价于 options.exec = SERVICE_EXEC_WORKER 的 *_ex 版本，保留为便捷接口。
 * handler 内的 reply() 线程安全，会在原 poller 上发送；请求 payload 已由
 * 框架拷贝，worker 中可安全读取。
 */
int service_add_method_worker(service_t *svc, const char *method, handler_fn fn, void *user);
int service_add_sync_method_worker(service_t *svc, const char *method,
                                        sync_handler_fn fn, void *user);

/* ---- 回复 ---- */
/* 一次性、异步回复。status 由应用定义；EVENT 请求上的回复会被静默忽略。 */
void reply(call_t *call, int status, const payload_t *payload);
/* 便捷版本：直接回复裸字节（encoding = 0）。 */
void reply_bytes(call_t *call, int status, const void *data, size_t len);
/* 分多次发送 response chunk；最后一个 chunk 必须带 STREAM_LAST。 */
int reply_stream(call_t *call, int status, const chunk_t *chunk);

/* ---- 调用上下文 ---- */
/*
 * 请求 deadline（绝对单调毫秒，0 = 无）。worker / inline handler 内均可读取；
 * 与 req->deadline_ms 同源。返回 0 表示客户端未设置超时。
 */
uint64_t call_deadline_ms(const call_t *call);
/*
 * 客户端是否已取消本次调用（best-effort）。取消无法抢占已运行的 handler，
 * 但 handler 可周期检查本函数尽快退出；排队中的 worker 任务会在启动前被跳过。
 */
int call_canceled(const call_t *call);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_H */
