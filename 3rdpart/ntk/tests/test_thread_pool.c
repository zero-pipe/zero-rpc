#include "ztk/thread/thread_pool.h"
#include "ztk/thread/sem.h"
#include <stdio.h>
#include <stdlib.h>

static ztk_sem *g_sem;
static volatile int g_sum;

static void add_task(void *user)
{
    int v = *(int *)user;
    g_sum += v;
    ztk_sem_post(g_sem, 1);
}

int main(void)
{
    g_sem = ztk_sem_create(0);
    if (!g_sem)
        return 1;

    ztk_thread_pool_opts_t opts;
    opts.thread_count = 2;
    opts.priority = ZTK_THREAD_PRIO_NORMAL;
    opts.auto_start = 1;

    ztk_thread_pool *pool = ztk_thread_pool_create(&opts);
    if (!pool) {
        ztk_sem_destroy(g_sem);
        return 1;
    }

    int a = 10, b = 20, c = 30;
    if (ztk_thread_pool_async(pool, add_task, &a, 1) != ZTK_OK ||
        ztk_thread_pool_async(pool, add_task, &b, 1) != ZTK_OK ||
        ztk_thread_pool_async_first(pool, add_task, &c, 1) != ZTK_OK) {
        fprintf(stderr, "async failed\n");
        ztk_thread_pool_destroy(pool);
        ztk_sem_destroy(g_sem);
        return 1;
    }

    for (int i = 0; i < 3; ++i) {
        if (ztk_sem_timedwait(g_sem, 5000) != ZTK_OK) {
            fprintf(stderr, "timeout\n");
            ztk_thread_pool_destroy(pool);
            ztk_sem_destroy(g_sem);
            return 1;
        }
    }

    if (g_sum != 60) {
        fprintf(stderr, "sum=%d expected 60\n", g_sum);
        ztk_thread_pool_destroy(pool);
        ztk_sem_destroy(g_sem);
        return 1;
    }

    ztk_thread_pool_destroy(pool);
    ztk_sem_destroy(g_sem);
    printf("test_thread_pool ok\n");
    return 0;
}
