#include "ztk/poller/poller_pool.h"
#include "ztk/poller/poller.h"
#include "ztk/util/buf.h"
#include "ztk/thread/thread.h"
#include "ztk/thread/sync.h"
#include <stdlib.h>
#include <string.h>

typedef struct ztk_poller_pool_slot {
    ztk_poller *poller;
    ztk_thread *thread;
    volatile int stop;
    ztk_buf_pool *buf_pool;
} ztk_poller_pool_slot;

struct ztk_poller_pool {
    ztk_poller_pool_slot *slots;
    unsigned count;
    unsigned thread_pos;
    int prefer_current;
    int started;
    ztk_thread_priority_t thread_priority;
    ztk_mutex *mtx;
};

static void pool_worker(void *arg)
{
    ztk_poller_pool_slot *slot = (ztk_poller_pool_slot *)arg;
    ztk_poller_run(slot->poller, &slot->stop);
}

ztk_poller_pool *ztk_poller_pool_create(const ztk_poller_pool_opts_t *opts)
{
    unsigned n = opts && opts->size ? opts->size : ztk_thread_hardware_concurrency();
    if (n < 1)
        n = 1;

    ztk_poller_pool *pool = (ztk_poller_pool *)calloc(1, sizeof(*pool));
    if (!pool)
        return NULL;

    pool->mtx = ztk_mutex_create(ZTK_MUTEX_NORMAL);
    if (!pool->mtx) {
        free(pool);
        return NULL;
    }

    pool->slots = (ztk_poller_pool_slot *)calloc(n, sizeof(*pool->slots));
    if (!pool->slots) {
        ztk_poller_pool_destroy(pool);
        return NULL;
    }
    pool->count = n;
    pool->prefer_current = opts && opts->prefer_current_thread ? 1 : 0;
    pool->thread_priority = ZTK_THREAD_PRIO_NORMAL;
    if (opts)
        pool->thread_priority = opts->thread_priority;
    pool->thread_pos = 0;

    for (unsigned i = 0; i < n; ++i) {
        pool->slots[i].poller = ztk_poller_create();
        if (!pool->slots[i].poller) {
            ztk_poller_pool_destroy(pool);
            return NULL;
        }
    }
    return pool;
}

void ztk_poller_pool_destroy(ztk_poller_pool *pool)
{
    if (!pool)
        return;
    /* 先停线程并排空 async（含无锁池跨线程 release 回投），再拆本地池 */
    ztk_poller_pool_stop(pool);
    ztk_poller_pool_detach_buf_pools(pool);
    if (pool->slots) {
        for (unsigned i = 0; i < pool->count; ++i) {
            if (pool->slots[i].poller)
                ztk_poller_destroy(pool->slots[i].poller);
        }
        free(pool->slots);
    }
    if (pool->mtx)
        ztk_mutex_destroy(pool->mtx);
    free(pool);
}

ztk_err_t ztk_poller_pool_start(ztk_poller_pool *pool)
{
    if (!pool || pool->started)
        return pool && pool->started ? ZTK_OK : ZTK_ERR_INVALID;

    for (unsigned i = 0; i < pool->count; ++i) {
        pool->slots[i].stop = 0;
        pool->slots[i].thread = ztk_thread_create_ex(pool_worker, &pool->slots[i], pool->thread_priority);
        if (!pool->slots[i].thread) {
            ztk_poller_pool_stop(pool);
            return ZTK_ERR_PLATFORM;
        }
    }
    pool->started = 1;
    return ZTK_OK;
}

void ztk_poller_pool_stop(ztk_poller_pool *pool)
{
    if (!pool || !pool->started)
        return;

    for (unsigned i = 0; i < pool->count; ++i)
        pool->slots[i].stop = 1;

    for (unsigned i = 0; i < pool->count; ++i) {
        if (pool->slots[i].thread) {
            ztk_thread_join(pool->slots[i].thread);
            ztk_thread_destroy(pool->slots[i].thread);
            pool->slots[i].thread = NULL;
        }
    }
    pool->started = 0;
}

unsigned ztk_poller_pool_size(const ztk_poller_pool *pool)
{
    return pool ? pool->count : 0;
}

ztk_poller *ztk_poller_pool_at(const ztk_poller_pool *pool, unsigned index)
{
    if (!pool || index >= pool->count)
        return NULL;
    return pool->slots[index].poller;
}

ztk_poller *ztk_poller_pool_get_first(const ztk_poller_pool *pool)
{
    return ztk_poller_pool_at(pool, 0);
}

static ztk_poller *find_current_poller(ztk_poller_pool *pool)
{
    for (unsigned i = 0; i < pool->count; ++i) {
        if (ztk_poller_is_current_thread(pool->slots[i].poller))
            return pool->slots[i].poller;
    }
    return NULL;
}

static uint64_t ztk_hash_u64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

ztk_poller *ztk_poller_pool_get_by_key(ztk_poller_pool *pool, uint64_t key)
{
    unsigned idx;
    if (!pool || pool->count == 0)
        return NULL;
    idx = (unsigned)(ztk_hash_u64(key) % pool->count);
    return pool->slots[idx].poller;
}

ztk_poller *ztk_poller_pool_get(ztk_poller_pool *pool, int prefer_current)
{
    if (!pool || pool->count == 0)
        return NULL;

    if (prefer_current && pool->prefer_current) {
        ztk_poller *cur = find_current_poller(pool);
        if (cur)
            return cur;
    }

    ztk_mutex_lock(pool->mtx);
    unsigned pos = pool->thread_pos;
    if (pos >= pool->count)
        pos = 0;

    ztk_poller *best = pool->slots[pos].poller;
    int min_load = ztk_poller_get_load(best);

    for (unsigned i = 0; i < pool->count; ++i) {
        unsigned idx = (pos + i) % pool->count;
        ztk_poller *p = pool->slots[idx].poller;
        int load = ztk_poller_get_load(p);
        if (load < min_load) {
            min_load = load;
            best = p;
            pos = idx;
        }
        if (min_load == 0)
            break;
    }
    pool->thread_pos = (pos + 1) % pool->count;
    ztk_mutex_unlock(pool->mtx);
    return best;
}

void ztk_poller_pool_set_prefer_current_thread(ztk_poller_pool *pool, int on)
{
    if (pool)
        pool->prefer_current = on ? 1 : 0;
}

unsigned ztk_poller_pool_get_loads(const ztk_poller_pool *pool, int *loads, unsigned loads_cap)
{
    if (!pool || !loads || loads_cap == 0)
        return 0;
    unsigned n = pool->count < loads_cap ? pool->count : loads_cap;
    for (unsigned i = 0; i < n; ++i)
        loads[i] = ztk_poller_get_load(pool->slots[i].poller);
    return n;
}

ztk_poller *ztk_tcp_pick_poller_from_pool(void *pool_user, ztk_poller *accept_poller)
{
    ztk_poller_pool *pool = (ztk_poller_pool *)pool_user;
    (void)accept_poller;
    if (!pool)
        return NULL;
    return ztk_poller_pool_get(pool, 0);
}

ztk_poller *ztk_tcp_pick_poller_by_key(void *pool_user, uint64_t key)
{
    ztk_poller_pool *pool = (ztk_poller_pool *)pool_user;
    if (!pool)
        return NULL;
    return ztk_poller_pool_get_by_key(pool, key);
}

ztk_err_t ztk_poller_pool_attach_buf_pools(ztk_poller_pool *pool, const ztk_buf_pool_opts *opts)
{
    unsigned i;

    if (!pool || !pool->slots) {
        return ZTK_ERR_INVALID;
    }

    for (i = 0; i < pool->count; ++i) {
        if (pool->slots[i].buf_pool) {
            continue;
        }
        pool->slots[i].buf_pool = ztk_buf_pool_create(opts);
        if (!pool->slots[i].buf_pool) {
            ztk_poller_pool_detach_buf_pools(pool);
            return ZTK_ERR_NOMEM;
        }
        ztk_poller_attach_buf_pool(pool->slots[i].poller, pool->slots[i].buf_pool);
    }
    return ZTK_OK;
}

void ztk_poller_pool_detach_buf_pools(ztk_poller_pool *pool)
{
    if (!pool || !pool->slots)
        return;

    for (unsigned i = 0; i < pool->count; ++i) {
        if (pool->slots[i].poller)
            ztk_poller_attach_buf_pool(pool->slots[i].poller, NULL);
        if (pool->slots[i].buf_pool) {
            ztk_buf_pool_destroy(pool->slots[i].buf_pool);
            pool->slots[i].buf_pool = NULL;
        }
    }
}
