#ifndef NTK_POLLER_POOL_H
#define NTK_POLLER_POOL_H

#include "../ntk_export.h"
#include "../ntk_errno.h"
#include "../thread/thread.h"
#include "../util/buf.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ntk_poller;
typedef struct ntk_poller ntk_poller;
typedef struct ntk_poller_pool ntk_poller_pool;

typedef struct ntk_poller_pool_opts {
    /** 0 表示 ntk_thread_hardware_concurrency()，至少为 1 */
    unsigned size;
    int prefer_current_thread;
    /** 工作线程调度优先级，默认 NTK_THREAD_PRIO_NORMAL */
    ntk_thread_priority_t thread_priority;
} ntk_poller_pool_opts_t;

NTK_API ntk_poller_pool *ntk_poller_pool_create(const ntk_poller_pool_opts_t *opts);
NTK_API void ntk_poller_pool_destroy(ntk_poller_pool *pool);

/** 为每个 poller 启动 ntk_poller_run 线程 */
NTK_API ntk_err_t ntk_poller_pool_start(ntk_poller_pool *pool);
NTK_API void ntk_poller_pool_stop(ntk_poller_pool *pool);

NTK_API unsigned ntk_poller_pool_size(const ntk_poller_pool *pool);
NTK_API ntk_poller *ntk_poller_pool_at(const ntk_poller_pool *pool, unsigned index);
NTK_API ntk_poller *ntk_poller_pool_get_first(const ntk_poller_pool *pool);

/**
 * 按负载选择 poller（对齐 EventPollerPool::getPoller）。
 * @param prefer_current 非 0 且当前线程属于池中某 poller 时直接返回该 poller
 */
NTK_API ntk_poller *ntk_poller_pool_get(ntk_poller_pool *pool, int prefer_current);

/** 按 conn_key 稳定 hash 到固定 poller（EventLoop Pinning） */
NTK_API ntk_poller *ntk_poller_pool_get_by_key(ntk_poller_pool *pool, uint64_t key);

NTK_API void ntk_poller_pool_set_prefer_current_thread(ntk_poller_pool *pool, int on);

/** 各 poller 负载 0–100；loads 长度至少 size，返回写入数量 */
NTK_API unsigned ntk_poller_pool_get_loads(const ntk_poller_pool *pool, int *loads, unsigned loads_cap);

/**
 * TcpServer pick_poller 默认实现：从 pool 选最轻载 poller（ignore accept_poller）。
 * user 须为 ntk_poller_pool*，prefer_current 由 pool 配置决定。
 */
NTK_API ntk_poller *ntk_tcp_pick_poller_from_pool(void *pool, ntk_poller *accept_poller);
NTK_API ntk_poller *ntk_tcp_pick_poller_by_key(void *pool, uint64_t key);

/** 为每个 poller 创建并挂载独立 ntk_buf_pool（per_poller / hybrid；建议 opts.thread_safe=0） */
NTK_API ntk_err_t ntk_poller_pool_attach_buf_pools(ntk_poller_pool *pool, const ntk_buf_pool_opts *opts);
NTK_API void ntk_poller_pool_detach_buf_pools(ntk_poller_pool *pool);

#ifdef __cplusplus
}
#endif

#endif /* NTK_POLLER_POOL_H */
