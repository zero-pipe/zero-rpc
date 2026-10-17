#include "poller/poller_pool.h"
#include "poller/poller.h"
#include "util/buf.h"
#include "thread/thread.h"
#include "thread/sync.h"
#include <stdlib.h>
#include <string.h>

typedef struct ntk_poller_pool_slot {
    ntk_poller *poller;
    ntk_thread *thread;
    volatile int stop;
    ntk_buf_pool *buf_pool;
} ntk_poller_pool_slot;

struct ntk_poller_pool {
    ntk_poller_pool_slot *slots;
    unsigned count;
    unsigned thread_pos;
    int prefer_current;
    int started;
    ntk_thread_priority_t thread_priority;
    ntk_mutex *mtx;
};

static void pool_worker(void *arg)
{
    ntk_poller_pool_slot *slot = (ntk_poller_pool_slot *)arg;
    ntk_poller_run(slot->poller, &slot->stop);
}

ntk_poller_pool *ntk_poller_pool_create(const ntk_poller_pool_opts_t *opts)
{
    unsigned n = opts && opts->size ? opts->size : ntk_thread_hardware_concurrency();
    if (n < 1)
        n = 1;

    ntk_poller_pool *pool = (ntk_poller_pool *)calloc(1, sizeof(*pool));
    if (!pool)
        return NULL;

    pool->mtx = ntk_mutex_create(NTK_MUTEX_NORMAL);
    if (!pool->mtx) {
        free(pool);
        return NULL;
    }

    pool->slots = (ntk_poller_pool_slot *)calloc(n, sizeof(*pool->slots));
    if (!pool->slots) {
        ntk_poller_pool_destroy(pool);
        return NULL;
    }
    pool->count = n;
    pool->prefer_current = opts && opts->prefer_current_thread ? 1 : 0;
    pool->thread_priority = NTK_THREAD_PRIO_NORMAL;
    if (opts)
        pool->thread_priority = opts->thread_priority;
    pool->thread_pos = 0;

    for (unsigned i = 0; i < n; ++i) {
        pool->slots[i].poller = ntk_poller_create();
        if (!pool->slots[i].poller) {
            ntk_poller_pool_destroy(pool);
            return NULL;
        }
    }
    return pool;
}

void ntk_poller_pool_destroy(ntk_poller_pool *pool)
{
    if (!pool)
        return;
    /* 先停线程并排空 async（含无锁池跨线程 release 回投），再拆本地池 */
    ntk_poller_pool_stop(pool);
    /*
     * 停线程后、拆本地池前，先真正排空各 poller 的 pending 任务队列。
     * 无锁池（thread_safe=0）在非 owner 线程的 release 会经 ntk_poller_async
     * 回投为延迟任务（pool_release_deferred），该任务持有 pool 指针；若在池
     * 被 detach 销毁后才排空，会访问已释放的池锁。此处先排空，确保所有回投
     * 在池仍存活时完成。
     */
    if (pool->slots) {
        for (unsigned i = 0; i < pool->count; ++i) {
            if (pool->slots[i].poller)
                ntk_poller_process_pending(pool->slots[i].poller);
        }
    }
    ntk_poller_pool_detach_buf_pools(pool);
    if (pool->slots) {
        for (unsigned i = 0; i < pool->count; ++i) {
            if (pool->slots[i].poller)
                ntk_poller_destroy(pool->slots[i].poller);
        }
        free(pool->slots);
    }
    if (pool->mtx)
        ntk_mutex_destroy(pool->mtx);
    free(pool);
}

ntk_err_t ntk_poller_pool_start(ntk_poller_pool *pool)
{
    if (!pool || pool->started)
        return pool && pool->started ? NTK_OK : NTK_ERR_INVALID;

    for (unsigned i = 0; i < pool->count; ++i) {
        pool->slots[i].stop = 0;
        pool->slots[i].thread = ntk_thread_create_ex(pool_worker, &pool->slots[i], pool->thread_priority);
        if (!pool->slots[i].thread) {
            ntk_poller_pool_stop(pool);
            return NTK_ERR_PLATFORM;
        }
    }
    pool->started = 1;
    return NTK_OK;
}

void ntk_poller_pool_stop(ntk_poller_pool *pool)
{
    if (!pool || !pool->started)
        return;

    for (unsigned i = 0; i < pool->count; ++i)
        pool->slots[i].stop = 1;

    for (unsigned i = 0; i < pool->count; ++i) {
        if (pool->slots[i].thread) {
            ntk_thread_join(pool->slots[i].thread);
            ntk_thread_destroy(pool->slots[i].thread);
            pool->slots[i].thread = NULL;
        }
    }
    pool->started = 0;
}

unsigned ntk_poller_pool_size(const ntk_poller_pool *pool)
{
    return pool ? pool->count : 0;
}

ntk_poller *ntk_poller_pool_at(const ntk_poller_pool *pool, unsigned index)
{
    if (!pool || index >= pool->count)
        return NULL;
    return pool->slots[index].poller;
}

ntk_poller *ntk_poller_pool_get_first(const ntk_poller_pool *pool)
{
    return ntk_poller_pool_at(pool, 0);
}

static ntk_poller *find_current_poller(ntk_poller_pool *pool)
{
    for (unsigned i = 0; i < pool->count; ++i) {
        if (ntk_poller_is_current_thread(pool->slots[i].poller))
            return pool->slots[i].poller;
    }
    return NULL;
}

static uint64_t ntk_hash_u64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

ntk_poller *ntk_poller_pool_get_by_key(ntk_poller_pool *pool, uint64_t key)
{
    unsigned idx;
    if (!pool || pool->count == 0)
        return NULL;
    idx = (unsigned)(ntk_hash_u64(key) % pool->count);
    return pool->slots[idx].poller;
}

ntk_poller *ntk_poller_pool_get(ntk_poller_pool *pool, int prefer_current)
{
    if (!pool || pool->count == 0)
        return NULL;

    if (prefer_current && pool->prefer_current) {
        ntk_poller *cur = find_current_poller(pool);
        if (cur)
            return cur;
    }

    ntk_mutex_lock(pool->mtx);
    unsigned pos = pool->thread_pos;
    if (pos >= pool->count)
        pos = 0;

    ntk_poller *best = pool->slots[pos].poller;
    int min_load = ntk_poller_get_load(best);

    for (unsigned i = 0; i < pool->count; ++i) {
        unsigned idx = (pos + i) % pool->count;
        ntk_poller *p = pool->slots[idx].poller;
        int load = ntk_poller_get_load(p);
        if (load < min_load) {
            min_load = load;
            best = p;
            pos = idx;
        }
        if (min_load == 0)
            break;
    }
    pool->thread_pos = (pos + 1) % pool->count;
    ntk_mutex_unlock(pool->mtx);
    return best;
}

void ntk_poller_pool_set_prefer_current_thread(ntk_poller_pool *pool, int on)
{
    if (pool)
        pool->prefer_current = on ? 1 : 0;
}

unsigned ntk_poller_pool_get_loads(const ntk_poller_pool *pool, int *loads, unsigned loads_cap)
{
    if (!pool || !loads || loads_cap == 0)
        return 0;
    unsigned n = pool->count < loads_cap ? pool->count : loads_cap;
    for (unsigned i = 0; i < n; ++i)
        loads[i] = ntk_poller_get_load(pool->slots[i].poller);
    return n;
}

ntk_poller *ntk_tcp_pick_poller_from_pool(void *pool_user, ntk_poller *accept_poller)
{
    ntk_poller_pool *pool = (ntk_poller_pool *)pool_user;
    (void)accept_poller;
    if (!pool)
        return NULL;
    return ntk_poller_pool_get(pool, 0);
}

ntk_poller *ntk_tcp_pick_poller_by_key(void *pool_user, uint64_t key)
{
    ntk_poller_pool *pool = (ntk_poller_pool *)pool_user;
    if (!pool)
        return NULL;
    return ntk_poller_pool_get_by_key(pool, key);
}

ntk_err_t ntk_poller_pool_attach_buf_pools(ntk_poller_pool *pool, const ntk_buf_pool_opts *opts)
{
    unsigned i;

    if (!pool || !pool->slots) {
        return NTK_ERR_INVALID;
    }

    for (i = 0; i < pool->count; ++i) {
        if (pool->slots[i].buf_pool) {
            continue;
        }
        pool->slots[i].buf_pool = ntk_buf_pool_create(opts);
        if (!pool->slots[i].buf_pool) {
            ntk_poller_pool_detach_buf_pools(pool);
            return NTK_ERR_NOMEM;
        }
        ntk_poller_attach_buf_pool(pool->slots[i].poller, pool->slots[i].buf_pool);
    }
    return NTK_OK;
}

void ntk_poller_pool_detach_buf_pools(ntk_poller_pool *pool)
{
    if (!pool || !pool->slots)
        return;

    for (unsigned i = 0; i < pool->count; ++i) {
        if (pool->slots[i].poller)
            ntk_poller_attach_buf_pool(pool->slots[i].poller, NULL);
        if (pool->slots[i].buf_pool) {
            ntk_buf_pool_destroy(pool->slots[i].buf_pool);
            pool->slots[i].buf_pool = NULL;
        }
    }
}
