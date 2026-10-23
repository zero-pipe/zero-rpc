#include "thread/thread_pool.h"
#include "platform.h"
#include "thread/sync.h"
#include "thread/sem.h"
#include <stdlib.h>

typedef struct ntk_thread_pool_task {
    ntk_thread_pool_fn fn;
    void *user;
    struct ntk_thread_pool_task *next;
} ntk_thread_pool_task;

struct ntk_thread_pool {
    ntk_mutex *mtx;
    ntk_sem *sem;
    ntk_thread_pool_task *head;
    ntk_thread_pool_task *tail;
    unsigned worker_count;
    ntk_thread **workers;
    uint64_t *worker_ids;
    ntk_thread_priority_t priority;
    volatile int shutdown;
    int started;
};

static int is_pool_worker(ntk_thread_pool *pool)
{
    uint64_t self = ntk_thread_self_id();
    for (unsigned i = 0; i < pool->worker_count; ++i) {
        if (pool->worker_ids[i] == self)
            return 1;
    }
    return 0;
}

static ntk_thread_pool_task *pop_task(ntk_thread_pool *pool)
{
    ntk_thread_pool_task *t = pool->head;
    if (t) {
        pool->head = t->next;
        if (!pool->head)
            pool->tail = NULL;
        t->next = NULL;
    }
    return t;
}

typedef struct ntk_thread_pool_worker_ctx {
    ntk_thread_pool *pool;
    unsigned index;
} ntk_thread_pool_worker_ctx;

static void pool_worker(void *arg)
{
    ntk_thread_pool_worker_ctx *wctx = (ntk_thread_pool_worker_ctx *)arg;
    ntk_thread_pool *pool = wctx->pool;
    if (wctx->index < pool->worker_count)
        pool->worker_ids[wctx->index] = ntk_thread_self_id();
    free(wctx);

    for (;;) {
        if (ntk_sem_wait(pool->sem) != NTK_OK)
            break;

        ntk_mutex_lock(pool->mtx);
        ntk_thread_pool_task *task = pop_task(pool);
        ntk_mutex_unlock(pool->mtx);

        if (!task)
            break;

        task->fn(task->user);
        free(task);
    }
}

static ntk_err_t push_task(ntk_thread_pool *pool, ntk_thread_pool_fn fn, void *user, int first)
{
    ntk_thread_pool_task *task = (ntk_thread_pool_task *)calloc(1, sizeof(*task));
    if (!task)
        return NTK_ERR_NOMEM;
    task->fn = fn;
    task->user = user;

    ntk_mutex_lock(pool->mtx);
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
    ntk_mutex_unlock(pool->mtx);

    return ntk_sem_post(pool->sem, 1);
}

ntk_thread_pool *ntk_thread_pool_create(const ntk_thread_pool_opts_t *opts)
{
    unsigned n = opts && opts->thread_count ? opts->thread_count : ntk_thread_hardware_concurrency();
    if (n < 1)
        n = 1;

    ntk_thread_pool *pool = (ntk_thread_pool *)calloc(1, sizeof(*pool));
    if (!pool)
        return NULL;

    pool->mtx = ntk_mutex_create(NTK_MUTEX_NORMAL);
    pool->sem = ntk_sem_create(0);
    if (!pool->mtx || !pool->sem) {
        ntk_thread_pool_destroy(pool);
        return NULL;
    }

    pool->worker_count = n;
    pool->priority = opts ? opts->priority : NTK_THREAD_PRIO_HIGHEST;
    pool->workers = (ntk_thread **)calloc(n, sizeof(ntk_thread *));
    pool->worker_ids = (uint64_t *)calloc(n, sizeof(uint64_t));
    if (!pool->workers || !pool->worker_ids) {
        ntk_thread_pool_destroy(pool);
        return NULL;
    }

    if (opts && opts->auto_start) {
        ntk_err_t err = ntk_thread_pool_start(pool);
        if (err != NTK_OK) {
            ntk_thread_pool_destroy(pool);
            return NULL;
        }
    }

    return pool;
}

static void join_workers(ntk_thread_pool *pool, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        if (pool->workers[i]) {
            ntk_thread_join(pool->workers[i]);
            ntk_thread_destroy(pool->workers[i]);
            pool->workers[i] = NULL;
        }
    }
}

ntk_err_t ntk_thread_pool_start(ntk_thread_pool *pool)
{
    unsigned created = 0;

    if (!pool || pool->started)
        return pool && pool->started ? NTK_OK : NTK_ERR_INVALID;

    for (unsigned i = 0; i < pool->worker_count; ++i) {
        ntk_thread_pool_worker_ctx *wctx = (ntk_thread_pool_worker_ctx *)calloc(1, sizeof(*wctx));
        if (!wctx) {
            join_workers(pool, created);
            return NTK_ERR_NOMEM;
        }
        wctx->pool = pool;
        wctx->index = i;
        pool->workers[i] = ntk_thread_create_ex(pool_worker, wctx, pool->priority);
        if (!pool->workers[i]) {
            free(wctx);
            join_workers(pool, created);
            return NTK_ERR_PLATFORM;
        }
        ++created;
    }
    pool->started = 1;
    return NTK_OK;
}

void ntk_thread_pool_fini(ntk_thread_pool *pool)
{
    if (!pool || !pool->started)
        return;

    pool->shutdown = 1;
    ntk_sem_post(pool->sem, pool->worker_count);

    join_workers(pool, pool->worker_count);
    pool->started = 0;
}

void ntk_thread_pool_destroy(ntk_thread_pool *pool)
{
    if (!pool)
        return;
    if (pool->started)
        ntk_thread_pool_fini(pool);

    ntk_thread_pool_task *t = pool->head;
    while (t) {
        ntk_thread_pool_task *next = t->next;
        free(t);
        t = next;
    }

    if (pool->workers)
        free(pool->workers);
    if (pool->worker_ids)
        free(pool->worker_ids);
    if (pool->sem)
        ntk_sem_destroy(pool->sem);
    if (pool->mtx)
        ntk_mutex_destroy(pool->mtx);
    free(pool);
}

ntk_err_t ntk_thread_pool_async(ntk_thread_pool *pool, ntk_thread_pool_fn fn, void *user, int may_sync)
{
    if (!pool || !fn)
        return NTK_ERR_INVALID;
    if (!pool->started)
        return NTK_ERR_STATE;
    if (may_sync && is_pool_worker(pool)) {
        fn(user);
        return NTK_OK;
    }
    return push_task(pool, fn, user, 0);
}

ntk_err_t ntk_thread_pool_async_first(ntk_thread_pool *pool, ntk_thread_pool_fn fn, void *user, int may_sync)
{
    if (!pool || !fn)
        return NTK_ERR_INVALID;
    if (!pool->started)
        return NTK_ERR_STATE;
    if (may_sync && is_pool_worker(pool)) {
        fn(user);
        return NTK_OK;
    }
    return push_task(pool, fn, user, 1);
}

unsigned ntk_thread_pool_worker_count(const ntk_thread_pool *pool)
{
    return pool ? pool->worker_count : 0;
}
