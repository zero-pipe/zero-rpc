#ifndef ZTK_THREAD_THREAD_POOL_H
#define ZTK_THREAD_THREAD_POOL_H

#include "../ztk_export.h"
#include "../ztk_errno.h"
#include "thread.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ztk_thread_pool ztk_thread_pool;
typedef void (*ztk_thread_pool_fn)(void *user);

typedef struct ztk_thread_pool_opts {
    /** 0 表示 ztk_thread_hardware_concurrency()，至少为 1 */
    unsigned thread_count;
    ztk_thread_priority_t priority;
    /** 非 0 时 create 后立即 start */
    int auto_start;
} ztk_thread_pool_opts_t;

ZTK_API ztk_thread_pool *ztk_thread_pool_create(const ztk_thread_pool_opts_t *opts);
ZTK_API void ztk_thread_pool_destroy(ztk_thread_pool *pool);

ZTK_API ztk_err_t ztk_thread_pool_start(ztk_thread_pool *pool);
ZTK_API void ztk_thread_pool_fini(ztk_thread_pool *pool);

/**
 * 投递任务；may_sync 非 0 且当前线程属于池内 worker 时同步执行。
 * @note user 须在 fn 执行完成前保持有效
 */
ZTK_API ztk_err_t ztk_thread_pool_async(ztk_thread_pool *pool, ztk_thread_pool_fn fn, void *user, int may_sync);
ZTK_API ztk_err_t ztk_thread_pool_async_first(ztk_thread_pool *pool, ztk_thread_pool_fn fn, void *user,
    int may_sync);

ZTK_API unsigned ztk_thread_pool_worker_count(const ztk_thread_pool *pool);

#ifdef __cplusplus
}
#endif

#endif /* ZTK_THREAD_THREAD_POOL_H */
