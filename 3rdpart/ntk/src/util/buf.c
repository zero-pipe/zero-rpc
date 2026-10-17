#include "util/buf.h"
#include "poller/poller.h"
#include "thread/sync.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <windows.h>
static int ntk_atomic_fetch_add(volatile long *v, int delta)
{
    return (int)InterlockedExchangeAdd(v, (long)delta);
}
#elif defined(__GNUC__) || defined(__clang__)
static int ntk_atomic_fetch_add(volatile int *v, int delta)
{
    return __atomic_fetch_add(v, delta, __ATOMIC_SEQ_CST);
}
#else
#error "ntk_buf requires MSVC or GCC/clang atomics"
#endif

/* ── ntk_buf：引用计数载荷 ── */

struct ntk_buf {
#if defined(_MSC_VER)
    volatile long refcnt;
#else
    volatile int refcnt;
#endif
    size_t len;
    size_t cap;
    ntk_buf_pool *pool;
    size_t pool_blk_cap;
    uint8_t data[];
};

static ntk_buf_pool *g_shared_pool;

void ntk_buf_set_shared_pool(ntk_buf_pool *pool)
{
    g_shared_pool = pool;
}

static ntk_buf *buf_create_malloc(size_t cap)
{
    ntk_buf *b = (ntk_buf *)malloc(sizeof(*b) + cap);
    if (!b) {
        return NULL;
    }
    b->refcnt = 1;
    b->len = 0;
    b->cap = cap;
    b->pool = NULL;
    b->pool_blk_cap = 0;
    return b;
}

static ntk_buf *buf_create_from_pool(ntk_buf_pool *pool, size_t cap)
{
    size_t hdr = offsetof(ntk_buf, data);
    size_t need = hdr + cap;
    size_t blk_cap = 0;
    void *mem;

    if (!pool) {
        return buf_create_malloc(cap);
    }

    mem = ntk_buf_pool_acquire(pool, need, &blk_cap);
    if (!mem) {
        return buf_create_malloc(cap);
    }

    {
        ntk_buf *b = (ntk_buf *)mem;
        b->refcnt = 1;
        b->len = 0;
        b->pool = pool;
        b->pool_blk_cap = blk_cap;
        b->cap = blk_cap > hdr ? blk_cap - hdr : cap;
        return b;
    }
}

ntk_buf *ntk_buf_alloc(size_t cap)
{
    if (cap == 0) {
        return NULL;
    }
    return buf_create_from_pool(g_shared_pool, cap);
}

ntk_buf *ntk_buf_alloc_local(ntk_poller *poller, size_t cap)
{
    ntk_buf_pool *local;

    if (cap == 0) {
        return NULL;
    }
    local = ntk_poller_buf_pool(poller);
    if (local) {
        return buf_create_from_pool(local, cap);
    }
    return ntk_buf_alloc(cap);
}

ntk_buf *ntk_buf_ref(ntk_buf *b)
{
    if (!b) {
        return NULL;
    }
    ntk_atomic_fetch_add(&b->refcnt, 1);
    return b;
}

ntk_buf *ntk_buf_try_ref(ntk_buf *b)
{
    if (!b) {
        return NULL;
    }
#if defined(_MSC_VER)
    for (;;) {
        long c = b->refcnt;
        if (c <= 0) {
            return NULL;
        }
        if (InterlockedCompareExchange(&b->refcnt, c + 1, c) == c) {
            return b;
        }
    }
#elif defined(__GNUC__) || defined(__clang__)
    for (;;) {
        int c = __atomic_load_n(&b->refcnt, __ATOMIC_ACQUIRE);
        if (c <= 0) {
            return NULL;
        }
        if (__atomic_compare_exchange_n(&b->refcnt, &c, c + 1, 0, __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST)) {
            return b;
        }
    }
#else
#error "ntk_buf_try_ref requires MSVC or GCC/clang atomics"
#endif
}

void ntk_buf_unref(ntk_buf *b)
{
    if (!b) {
        return;
    }
    if (ntk_atomic_fetch_add(&b->refcnt, -1) == 1) {
        if (b->pool) {
            ntk_buf_pool_release(b->pool, b, b->pool_blk_cap);
        } else {
            free(b);
        }
    }
}

const void *ntk_buf_data(const ntk_buf *b)
{
    return b ? (const void *)b->data : NULL;
}

size_t ntk_buf_len(const ntk_buf *b)
{
    return b ? b->len : 0;
}

size_t ntk_buf_cap(const ntk_buf *b)
{
    return b ? b->cap : 0;
}

int ntk_buf_refcnt(const ntk_buf *b)
{
    if (!b) {
        return 0;
    }
#if defined(_MSC_VER)
    return (int)b->refcnt;
#else
    return b->refcnt;
#endif
}

void ntk_buf_set_len(ntk_buf *b, size_t len)
{
    if (!b || len > b->cap) {
        return;
    }
    b->len = len;
}

/* ── ntk_buf_pool：分档侵入式 freelist ── */

#define NTK_BUF_BUCKET_COUNT 9

/* 512 → 2K → 8K → 32K → 64K → 128K → 512K → 1M → 2M */
static const size_t k_bucket_caps[NTK_BUF_BUCKET_COUNT] = {
    512, 2048, 8192, 32768, 65536, 131072, 524288, 1048576, 2097152,
};

typedef struct ntk_buf_free_node {
    struct ntk_buf_free_node *next;
} ntk_buf_free_node;

struct ntk_buf_pool {
    unsigned max_per_bucket;
    int thread_safe;
    ntk_buf_free_node *free_heads[NTK_BUF_BUCKET_COUNT];
    unsigned counts[NTK_BUF_BUCKET_COUNT];
    ntk_buf_pool_stats stats;
    ntk_mutex *mu; /* thread_safe=0 时为 NULL */
    ntk_poller *owner; /* 无锁本地池所属 poller；跨线程 release 回投 */
};

typedef struct ntk_buf_pool_defer {
    ntk_buf_pool *pool;
    void *ptr;
    size_t cap;
} ntk_buf_pool_defer;

void ntk_buf_pool_set_owner_poller(ntk_buf_pool *pool, ntk_poller *owner)
{
    if (pool) {
        pool->owner = owner;
    }
}

static int bucket_index_for_size(size_t size, size_t *out_cap)
{
    int i;

    for (i = 0; i < NTK_BUF_BUCKET_COUNT; ++i) {
        if (size <= k_bucket_caps[i]) {
            if (out_cap) {
                *out_cap = k_bucket_caps[i];
            }
            return i;
        }
    }
    return -1;
}

static int bucket_index_exact_cap(size_t cap)
{
    int i;

    for (i = 0; i < NTK_BUF_BUCKET_COUNT; ++i) {
        if (cap == k_bucket_caps[i]) {
            return i;
        }
    }
    return -1;
}

static void pool_lock(ntk_buf_pool *pool)
{
    if (pool && pool->mu) {
        ntk_mutex_lock(pool->mu);
    }
}

static void pool_unlock(ntk_buf_pool *pool)
{
    if (pool && pool->mu) {
        ntk_mutex_unlock(pool->mu);
    }
}

static void pool_release_now(ntk_buf_pool *pool, void *ptr, size_t cap)
{
    int bi;
    ntk_buf_free_node *node;

    bi = bucket_index_exact_cap(cap);
    if (bi < 0) {
        pool_lock(pool);
        pool->stats.release_dropped++;
        pool_unlock(pool);
        free(ptr);
        return;
    }

    pool_lock(pool);
    if (pool->counts[bi] < pool->max_per_bucket) {
        node = (ntk_buf_free_node *)ptr;
        node->next = pool->free_heads[bi];
        pool->free_heads[bi] = node;
        pool->counts[bi]++;
        pool->stats.release_cached++;
        pool_unlock(pool);
        return;
    }
    pool->stats.release_dropped++;
    pool_unlock(pool);
    free(ptr);
}

static void pool_release_deferred(void *user)
{
    ntk_buf_pool_defer *d = (ntk_buf_pool_defer *)user;

    if (!d) {
        return;
    }
    if (d->pool && d->ptr) {
        pool_release_now(d->pool, d->ptr, d->cap);
    }
    free(d);
}

ntk_buf_pool *ntk_buf_pool_create(const ntk_buf_pool_opts *opts)
{
    ntk_buf_pool *p = (ntk_buf_pool *)calloc(1, sizeof(*p));
    int thread_safe = 1;

    if (!p) {
        return NULL;
    }
    p->max_per_bucket =
        (opts && opts->max_per_bucket > 0) ? opts->max_per_bucket : NTK_BUF_POOL_DEFAULT_MAX;
    if (opts) {
        thread_safe = opts->thread_safe ? 1 : 0;
    }
    p->thread_safe = thread_safe;
    if (thread_safe) {
        p->mu = ntk_mutex_create(NTK_MUTEX_NORMAL);
        if (!p->mu) {
            free(p);
            return NULL;
        }
    }
    return p;
}

void ntk_buf_pool_destroy(ntk_buf_pool *pool)
{
    int i;

    if (!pool) {
        return;
    }
    pool->owner = NULL;
    pool_lock(pool);
    for (i = 0; i < NTK_BUF_BUCKET_COUNT; ++i) {
        ntk_buf_free_node *n = pool->free_heads[i];
        while (n) {
            ntk_buf_free_node *next = n->next;
            free(n);
            n = next;
        }
        pool->free_heads[i] = NULL;
        pool->counts[i] = 0;
    }
    pool_unlock(pool);
    if (pool->mu) {
        ntk_mutex_destroy(pool->mu);
    }
    free(pool);
}

void *ntk_buf_pool_acquire(ntk_buf_pool *pool, size_t size, size_t *out_cap)
{
    size_t cap = 0;
    int bi;
    void *ptr = NULL;

    if (!pool || size == 0) {
        return NULL;
    }

    bi = bucket_index_for_size(size, &cap);
    if (bi < 0) {
        void *p = malloc(size);
        pool_lock(pool);
        pool->stats.acquire_oversize++;
        pool_unlock(pool);
        if (out_cap) {
            *out_cap = size;
        }
        return p;
    }

    pool_lock(pool);
    if (pool->free_heads[bi]) {
        ntk_buf_free_node *n = pool->free_heads[bi];
        pool->free_heads[bi] = n->next;
        pool->counts[bi]--;
        pool->stats.acquire_hit++;
        ptr = n;
    } else {
        pool->stats.acquire_miss++;
    }
    pool_unlock(pool);

    if (!ptr) {
        ptr = malloc(cap);
        if (!ptr) {
            return NULL;
        }
    }
    if (out_cap) {
        *out_cap = cap;
    }
    return ptr;
}

void ntk_buf_pool_release(ntk_buf_pool *pool, void *ptr, size_t cap)
{
    ntk_buf_pool_defer *d;

    if (!pool || !ptr) {
        return;
    }

    /* 无锁本地池：非 owner 线程末次释放 → 回投 owner；失败则 free，不踩 freelist */
    if (!pool->thread_safe && pool->owner && !ntk_poller_is_current_thread(pool->owner)) {
        d = (ntk_buf_pool_defer *)malloc(sizeof(*d));
        if (!d) {
            free(ptr);
            return;
        }
        d->pool = pool;
        d->ptr = ptr;
        d->cap = cap;
        if (ntk_poller_async(pool->owner, pool_release_deferred, d, 0) != NTK_OK) {
            free(ptr);
            free(d);
        }
        return;
    }

    pool_release_now(pool, ptr, cap);
}

void ntk_buf_pool_get_stats(const ntk_buf_pool *pool, ntk_buf_pool_stats *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!pool) {
        return;
    }
    pool_lock((ntk_buf_pool *)pool);
    *out = pool->stats;
    pool_unlock((ntk_buf_pool *)pool);
}

void ntk_buf_pool_reset_stats(ntk_buf_pool *pool)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    memset(&pool->stats, 0, sizeof(pool->stats));
    pool_unlock(pool);
}

int ntk_buf_pool_bucket_counts(const ntk_buf_pool *pool, unsigned *out_counts, int max_buckets)
{
    int n;
    int i;

    if (!pool || !out_counts || max_buckets <= 0) {
        return 0;
    }
    n = max_buckets < NTK_BUF_BUCKET_COUNT ? max_buckets : NTK_BUF_BUCKET_COUNT;
    pool_lock((ntk_buf_pool *)pool);
    for (i = 0; i < n; ++i) {
        out_counts[i] = pool->counts[i];
    }
    pool_unlock((ntk_buf_pool *)pool);
    return n;
}
