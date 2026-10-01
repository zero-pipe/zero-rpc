#include "ztk/thread/thread_pool.h"
#include "ztk/platform.h"
#include "ztk/thread/sync.h"
#include "ztk/thread/sem.h"
#include <stdlib.h>

typedef struct ztk_thread_pool_task {
    ztk_thread_pool_fn fn;
    void *user;
    struct ztk_thread_pool_task *next;
} ztk_thread_pool_task;

struct ztk_thread_pool {
    ztk_mutex *mtx;
    ztk_sem *sem;
    ztk_thread_pool_task *head;
    ztk_thread_pool_task *tail;
    unsigned worker_count;
    ztk_thread **workers;
    uint64_t *worker_ids;
    ztk_thread_priority_t priority;
    volatile int shutdown;
    int started;
};

static int is_pool_worker(ztk_thread_pool *pool)
{
    uint64_t self = ztk_thread_self_id();
    for (unsigned i = 0; i < pool->worker_count; ++i) {
        if (pool->worker_ids[i] == self)
            return 1;
    }
    return 0;
}

static ztk_thread_pool_task *pop_task(ztk_thread_pool *pool)
{
    ztk_thread_pool_task *t = pool->head;
    if (t) {
        pool->head = t->next;
        if (!pool->head)
            pool->tail = NULL;
        t->next = NULL;
    }
    return t;
}

typedef struct ztk_thread_pool_worker_ctx {
    ztk_thread_pool *pool;
    unsigned index;
} ztk_thread_pool_worker_ctx;

static void pool_worker(void *arg)
{
    ztk_thread_pool_worker_ctx *wctx = (ztk_thread_pool_worker_ctx *)arg;
    ztk_thread_pool *pool = wctx->pool;
    if (wctx->index < pool->worker_count)
        pool->worker_ids[wctx->index] = ztk_thread_self_id();
    free(wctx);

    for (;;) {
        if (ztk_sem_wait(pool->sem) != ZTK_OK)
            break;

        ztk_mutex_lock(pool->mtx);
        ztk_thread_pool_task *task = pop_task(pool);
        ztk_mutex_unlock(pool->mtx);

        if (!task)
            break;

        task->fn(task->user);
    }
}

static ztk_err_t push_task(ztk_thread_pool *pool, ztk_thread_pool_fn fn, void *user, int first)
{
    ztk_thread_pool_task *task = (ztk_thread_pool_task *)calloc(1, sizeof(*task));
    if (!task)
        return ZTK_ERR_NOMEM;
    task->fn = fn;
    task->user = user;

    ztk_mutex_lock(pool->mtx);
    if (first) {
        task->next = pool->head;
        pool->head = task;
        if (!pool->tail)
            pool->tail = task;
    } else {
        task->next = NULL;
        if (pool->tail)
            pool->tail->next = task;
        else
            pool->head = task;
        pool->tail = task;
    }
    ztk_mutex_unlock(pool->mtx);

    return ztk_sem_post(pool->sem, 1);
}

ztk_thread_pool *ztk_thread_pool_create(const ztk_thread_pool_opts_t *opts)
{
    unsigned n = opts && opts->thread_count ? opts->thread_count : ztk_thread_hardware_concurrency();
    if (n < 1)
        n = 1;

    ztk_thread_pool *pool = (ztk_thread_pool *)calloc(1, sizeof(*pool));
    if (!pool)
        return NULL;

    pool->mtx = ztk_mutex_create(ZTK_MUTEX_NORMAL);
    pool->sem = ztk_sem_create(0);
    if (!pool->mtx || !pool->sem) {
        ztk_thread_pool_destroy(pool);
        return NULL;
    }

    pool->worker_count = n;
    pool->priority = opts ? opts->priority : ZTK_THREAD_PRIO_HIGHEST;
    pool->workers = (ztk_thread **)calloc(n, sizeof(ztk_thread *));
    pool->worker_ids = (uint64_t *)calloc(n, sizeof(uint64_t));
    if (!pool->workers || !pool->worker_ids) {
        ztk_thread_pool_destroy(pool);
        return NULL;
    }

    if (opts && opts->auto_start) {
        ztk_err_t err = ztk_thread_pool_start(pool);
        if (err != ZTK_OK) {
            ztk_thread_pool_destroy(pool);
            return NULL;
        }
    }

    return pool;
}

static void join_workers(ztk_thread_pool *pool, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        if (pool->workers[i]) {
            ztk_thread_join(pool->workers[i]);
            ztk_thread_destroy(pool->workers[i]);
            pool->workers[i] = NULL;
        }
    }
}

ztk_err_t ztk_thread_pool_start(ztk_thread_pool *pool)
{
    unsigned created = 0;

    if (!pool || pool->started)
        return pool && pool->started ? ZTK_OK : ZTK_ERR_INVALID;

    for (unsigned i = 0; i < pool->worker_count; ++i) {
        ztk_thread_pool_worker_ctx *wctx = (ztk_thread_pool_worker_ctx *)calloc(1, sizeof(*wctx));
        if (!wctx) {
            join_workers(pool, created);
            return ZTK_ERR_NOMEM;
        }
        wctx->pool = pool;
        wctx->index = i;
        pool->workers[i] = ztk_thread_create_ex(pool_worker, wctx, pool->priority);
        if (!pool->workers[i]) {
            free(wctx);
            join_workers(pool, created);
            return ZTK_ERR_PLATFORM;
        }
        ++created;
    }
    pool->started = 1;
    return ZTK_OK;
}

void ztk_thread_pool_fini(ztk_thread_pool *pool)
{
    if (!pool || !pool->started)
        return;

    pool->shutdown = 1;
    ztk_sem_post(pool->sem, pool->worker_count);

    join_workers(pool, pool->worker_count);
    pool->started = 0;
}

void ztk_thread_pool_destroy(ztk_thread_pool *pool)
{
    if (!pool)
        return;
    if (pool->started)
        ztk_thread_pool_fini(pool);

    ztk_thread_pool_task *t = pool->head;
    while (t) {
        ztk_thread_pool_task *next = t->next;
        free(t);
        t = next;
    }

    if (pool->workers)
        free(pool->workers);
    if (pool->worker_ids)
        free(pool->worker_ids);
    if (pool->sem)
        ztk_sem_destroy(pool->sem);
    if (pool->mtx)
        ztk_mutex_destroy(pool->mtx);
    free(pool);
}

ztk_err_t ztk_thread_pool_async(ztk_thread_pool *pool, ztk_thread_pool_fn fn, void *user, int may_sync)
{
    if (!pool || !fn)
        return ZTK_ERR_INVALID;
    if (!pool->started)
        return ZTK_ERR_STATE;
    if (may_sync && is_pool_worker(pool)) {
        fn(user);
        return ZTK_OK;
    }
    return push_task(pool, fn, user, 0);
}

ztk_err_t ztk_thread_pool_async_first(ztk_thread_pool *pool, ztk_thread_pool_fn fn, void *user, int may_sync)
{
    if (!pool || !fn)
        return ZTK_ERR_INVALID;
    if (!pool->started)
        return ZTK_ERR_STATE;
    if (may_sync && is_pool_worker(pool)) {
        fn(user);
        return ZTK_OK;
    }
    return push_task(pool, fn, user, 1);
}

unsigned ztk_thread_pool_worker_count(const ztk_thread_pool *pool)
{
    return pool ? pool->worker_count : 0;
}
