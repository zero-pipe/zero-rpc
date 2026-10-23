#ifndef NTK_THREAD_THREAD_POOL_H
#define NTK_THREAD_THREAD_POOL_H

#include "../ntk_export.h"
#include "../ntk_errno.h"
#include "thread.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ntk_thread_pool ntk_thread_pool;
typedef void (*ntk_thread_pool_fn)(void *user);

typedef struct ntk_thread_pool_opts {
    /** 0 表示 ntk_thread_hardware_concurrency()，至少为 1 */
    unsigned thread_count;
    ntk_thread_priority_t priority;
    /** 非 0 时 create 后立即 start */
    int auto_start;
} ntk_thread_pool_opts_t;

NTK_API ntk_thread_pool *ntk_thread_pool_create(const ntk_thread_pool_opts_t *opts);
NTK_API void ntk_thread_pool_destroy(ntk_thread_pool *pool);

NTK_API ntk_err_t ntk_thread_pool_start(ntk_thread_pool *pool);
NTK_API void ntk_thread_pool_fini(ntk_thread_pool *pool);

/**
 * 投递任务；may_sync 非 0 且当前线程属于池内 worker 时同步执行。
 * @note user 须在 fn 执行完成前保持有效
 */
NTK_API ntk_err_t ntk_thread_pool_async(ntk_thread_pool *pool, ntk_thread_pool_fn fn, void *user, int may_sync);
NTK_API ntk_err_t ntk_thread_pool_async_first(ntk_thread_pool *pool, ntk_thread_pool_fn fn, void *user,
    int may_sync);

NTK_API unsigned ntk_thread_pool_worker_count(const ntk_thread_pool *pool);

#ifdef __cplusplus
}
#endif

#endif /* NTK_THREAD_THREAD_POOL_H */
