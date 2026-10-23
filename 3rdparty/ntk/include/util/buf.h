#ifndef NTK_UTIL_BUF_H
#define NTK_UTIL_BUF_H

/**
 * 媒体字节内存（勿与 ntk_byte_buf 混淆）：
 *
 * 1) ntk_buf + ntk_buf_pool — 引用计数载荷；alloc 可走共享池或 per-poller 池
 * 2) ntk_buf_pool_acquire/release — 裸指针槽位（ingress 工作区等单所有者）
 *
 * 选用：
 *   跨 poller 共享（ring）     → ntk_buf_alloc / ntk_buf_set_shared_pool
 *   同 poller 连接内（HTTP 发送）→ ntk_buf_alloc_local(poller)
 *
 * 线程约定：
 *   - shared 池（thread_safe=1）：acquire/release 带锁，可跨线程
 *   - poller 本地池（thread_safe=0）：acquire 仅所属 poller 线程；
 *     末次 unref / release 若不在所属线程，会 async 回 owner 再入池
 *     （async 失败则 free，不踩无锁 freelist）
 *   - 跨 poller 共享载荷（GOP ring）请用 ntk_buf_alloc，勿用 alloc_local
 *   - ntk_buf_alloc_local 须传会话钉住的 poller，勿跨 poller 乱用
 */
#include "../ntk_export.h"
#include "../ntk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ntk_poller;
typedef struct ntk_poller ntk_poller;

typedef struct ntk_buf ntk_buf;
typedef struct ntk_buf_pool ntk_buf_pool;

/* ── 引用计数载荷 ── */

/** 安装跨 poller 共享池；NULL 时 ntk_buf_alloc 回退 malloc */
NTK_API void ntk_buf_set_shared_pool(ntk_buf_pool *pool);

/** 共享路径：ring / 多读者（走 shared pool 或 malloc） */
NTK_API ntk_buf *ntk_buf_alloc(size_t cap);
/** 本地路径：session 所属 poller 池 → 共享池 → malloc */
NTK_API ntk_buf *ntk_buf_alloc_local(ntk_poller *poller, size_t cap);

NTK_API ntk_buf *ntk_buf_ref(ntk_buf *b);
/**
 * @brief 仅当 refcnt>0 时原子 +1；对象已销毁或并发末次 unref 时返回 NULL。
 * @note 用于无锁读路径：先 load 指针再 try_ref，失败则重试/回退。
 */
NTK_API ntk_buf *ntk_buf_try_ref(ntk_buf *b);
NTK_API void ntk_buf_unref(ntk_buf *b);

NTK_API const void *ntk_buf_data(const ntk_buf *b);
NTK_API size_t ntk_buf_len(const ntk_buf *b);
NTK_API size_t ntk_buf_cap(const ntk_buf *b);
NTK_API int ntk_buf_refcnt(const ntk_buf *b);
NTK_API void ntk_buf_set_len(ntk_buf *b, size_t len);

/* ── 分档裸内存池 ── */

#define NTK_BUF_POOL_DEFAULT_MAX 128

/**
 * 分档容量（字节）：512, 2K, 8K, 32K, 64K, 128K, 512K, 1M, 2M。
 * 超过最大档走 malloc，不入池（计入 acquire_oversize）。
 * 每档空闲块用侵入式 freelist，池对象体积与 max_per_bucket 无关。
 */
typedef struct ntk_buf_pool_opts {
    unsigned max_per_bucket;
    /**
     * 非 0（默认）：acquire/release 加锁，可跨线程（全局共享池）。
     * 0：无锁，仅单线程访问（poller 本地池）。
     */
    int thread_safe;
} ntk_buf_pool_opts;

/** 池命中统计（轻量计数；热路径一次自增，可忽略） */
typedef struct ntk_buf_pool_stats {
    uint64_t acquire_hit;      /**< freelist 命中 */
    uint64_t acquire_miss;     /**< 档内无空闲，malloc 新块 */
    uint64_t acquire_oversize; /**< 超过最大档，直接 malloc */
    uint64_t release_cached;   /**< 归还入 freelist */
    uint64_t release_dropped;  /**< freelist 已满或非档容量，free */
} ntk_buf_pool_stats;

NTK_API ntk_buf_pool *ntk_buf_pool_create(const ntk_buf_pool_opts *opts);
NTK_API void ntk_buf_pool_destroy(ntk_buf_pool *pool);
NTK_API void *ntk_buf_pool_acquire(ntk_buf_pool *pool, size_t size, size_t *out_cap);
NTK_API void ntk_buf_pool_release(ntk_buf_pool *pool, void *ptr, size_t cap);

NTK_API void ntk_buf_pool_get_stats(const ntk_buf_pool *pool, ntk_buf_pool_stats *out);
NTK_API void ntk_buf_pool_reset_stats(ntk_buf_pool *pool);
/** 各档当前 freelist 长度；out_counts 至少 9 个；返回档位数 */
NTK_API int ntk_buf_pool_bucket_counts(const ntk_buf_pool *pool, unsigned *out_counts,
                                       int max_buckets);

/* ── poller 本地池（per_poller / hybrid） ── */

/**
 * 挂载/解绑 poller 本地池。attach 会绑定 pool→owner poller，供无锁池跨线程
 * release 回投；detach（pool=NULL）前应已排空该 poller 任务队列。
 */
NTK_API void ntk_poller_attach_buf_pool(ntk_poller *poller, ntk_buf_pool *pool);
NTK_API ntk_buf_pool *ntk_poller_buf_pool(ntk_poller *poller);

#ifdef __cplusplus
}
#endif

#endif /* NTK_UTIL_BUF_H */
