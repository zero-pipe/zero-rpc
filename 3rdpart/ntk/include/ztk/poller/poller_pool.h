#ifndef ZTK_POLLER_POOL_H
#define ZTK_POLLER_POOL_H

#include "../ztk_export.h"
#include "../ztk_errno.h"
#include "../thread/thread.h"
#include "../util/buf.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ztk_poller;
typedef struct ztk_poller ztk_poller;
typedef struct ztk_poller_pool ztk_poller_pool;

typedef struct ztk_poller_pool_opts {
    /** 0 表示 ztk_thread_hardware_concurrency()，至少为 1 */
    unsigned size;
    int prefer_current_thread;
    /** 工作线程调度优先级，默认 ZTK_THREAD_PRIO_NORMAL */
    ztk_thread_priority_t thread_priority;
} ztk_poller_pool_opts_t;

ZTK_API ztk_poller_pool *ztk_poller_pool_create(const ztk_poller_pool_opts_t *opts);
ZTK_API void ztk_poller_pool_destroy(ztk_poller_pool *pool);

/** 为每个 poller 启动 ztk_poller_run 线程 */
ZTK_API ztk_err_t ztk_poller_pool_start(ztk_poller_pool *pool);
ZTK_API void ztk_poller_pool_stop(ztk_poller_pool *pool);

ZTK_API unsigned ztk_poller_pool_size(const ztk_poller_pool *pool);
ZTK_API ztk_poller *ztk_poller_pool_at(const ztk_poller_pool *pool, unsigned index);
ZTK_API ztk_poller *ztk_poller_pool_get_first(const ztk_poller_pool *pool);

/**
 * 按负载选择 poller（对齐 EventPollerPool::getPoller）。
 * @param prefer_current 非 0 且当前线程属于池中某 poller 时直接返回该 poller
 */
ZTK_API ztk_poller *ztk_poller_pool_get(ztk_poller_pool *pool, int prefer_current);

/** 按 conn_key 稳定 hash 到固定 poller（EventLoop Pinning） */
ZTK_API ztk_poller *ztk_poller_pool_get_by_key(ztk_poller_pool *pool, uint64_t key);

ZTK_API void ztk_poller_pool_set_prefer_current_thread(ztk_poller_pool *pool, int on);

/** 各 poller 负载 0–100；loads 长度至少 size，返回写入数量 */
ZTK_API unsigned ztk_poller_pool_get_loads(const ztk_poller_pool *pool, int *loads, unsigned loads_cap);

/**
 * TcpServer pick_poller 默认实现：从 pool 选最轻载 poller（ignore accept_poller）。
 * user 须为 ztk_poller_pool*，prefer_current 由 pool 配置决定。
 */
ZTK_API ztk_poller *ztk_tcp_pick_poller_from_pool(void *pool, ztk_poller *accept_poller);
ZTK_API ztk_poller *ztk_tcp_pick_poller_by_key(void *pool, uint64_t key);

/** 为每个 poller 创建并挂载独立 ztk_buf_pool（per_poller / hybrid；建议 opts.thread_safe=0） */
ZTK_API ztk_err_t ztk_poller_pool_attach_buf_pools(ztk_poller_pool *pool, const ztk_buf_pool_opts *opts);
ZTK_API void ztk_poller_pool_detach_buf_pools(ztk_poller_pool *pool);

#ifdef __cplusplus
}
#endif

#endif /* ZTK_POLLER_POOL_H */
