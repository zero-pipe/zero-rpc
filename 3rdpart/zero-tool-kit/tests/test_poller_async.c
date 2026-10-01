#include "ztk/poller/poller.h"
#include "ztk/thread/thread.h"
#include "ztk/thread/sem.h"
#include <stdio.h>
#include "test_portable.h"

static ztk_poller *g_poller;
static ztk_sem *g_go;
static volatile int g_stop;
static volatile int g_value;
static volatile int g_ran_on_poller_thread;

static void set_value(void *user)
{
    int v = *(int *)user;
    g_value = v;
    g_ran_on_poller_thread = ztk_poller_is_current_thread(g_poller);
    g_stop = 1;
}

static int g_async_value = 42;

static void worker(void *user)
{
    (void)user;
    ztk_sem_wait(g_go);
    g_async_value = 42;
    if (ztk_poller_async(g_poller, set_value, &g_async_value, 0) != ZTK_OK)
        g_stop = 1;
}

static int test_may_sync(void)
{
    g_poller = ztk_poller_create();
    if (!g_poller)
        return 1;

    ztk_poller_bind_thread(g_poller);
    g_value = 0;
    int v = 7;
    if (ztk_poller_async(g_poller, set_value, &v, 1) != ZTK_OK || g_value != 7)
        return 1;
    if (!ztk_poller_is_current_thread(g_poller))
        return 1;

    ztk_poller_unbind_thread(g_poller);
    ztk_poller_destroy(g_poller);
    g_poller = NULL;
    return 0;
}

static int test_cross_thread(void)
{
    g_poller = ztk_poller_create();
    g_go = ztk_sem_create(0);
    if (!g_poller || !g_go)
        return 1;

    g_stop = 0;
    g_value = 0;
    g_ran_on_poller_thread = 0;

    ztk_thread *t = ztk_thread_create(worker, NULL);
    if (!t) {
        ztk_poller_destroy(g_poller);
        ztk_sem_destroy(g_go);
        return 1;
    }

    ztk_poller_bind_thread(g_poller);
    ztk_sem_post(g_go, 1);

    for (int i = 0; i < 500 && !g_stop; ++i)
        ztk_poller_poll(g_poller, 20);

    ztk_poller_unbind_thread(g_poller);
    ztk_thread_join(t);
    ztk_thread_destroy(t);
    ztk_poller_destroy(g_poller);
    ztk_sem_destroy(g_go);
    g_poller = NULL;
    g_go = NULL;

    if (g_value != 42 || !g_ran_on_poller_thread)
        return 1;
    return 0;
}

static int g_seq[2];
static int g_seq_idx;

static void mark_first(void *user)
{
    (void)user;
    g_seq[g_seq_idx++] = 1;
}

static void mark_second(void *user)
{
    (void)user;
    g_seq[g_seq_idx++] = 2;
}

static int test_async_first_order(void)
{
    ztk_poller *p = ztk_poller_create();
    if (!p)
        return 1;

    ztk_poller_bind_thread(p);
    g_seq_idx = 0;
    g_seq[0] = g_seq[1] = 0;

    ztk_poller_async(p, mark_second, NULL, 0);
    ztk_poller_async_first(p, mark_first, NULL, 0);
    ztk_poller_process_pending(p);

    ztk_poller_unbind_thread(p);
    ztk_poller_destroy(p);

    return (g_seq_idx == 2 && g_seq[0] == 1 && g_seq[1] == 2) ? 0 : 1;
}

static volatile int g_run_loops;

static void poller_thread_main(void *user)
{
    ztk_poller *p = (ztk_poller *)user;
    ztk_poller_run(p, &g_stop);
    g_run_loops = 1;
}

static void worker_for_run(void *user)
{
    (void)user;
    ztk_test_sleep_ms(50);
    g_async_value = 42;
    ztk_poller_async(g_poller, set_value, &g_async_value, 0);
}

static int test_poller_run_api(void)
{
    ztk_poller *p = ztk_poller_create();
    if (!p)
        return 1;

    g_poller = p;
    g_stop = 0;
    g_value = 0;
    g_run_loops = 0;

    ztk_thread *pt = ztk_thread_create(poller_thread_main, p);
    ztk_thread *wt = ztk_thread_create(worker_for_run, NULL);
    if (!pt || !wt) {
        g_stop = 1;
        ztk_poller_destroy(p);
        return 1;
    }

    ztk_thread_join(wt);
    ztk_thread_destroy(wt);
    ztk_thread_join(pt);
    ztk_thread_destroy(pt);
    ztk_poller_destroy(p);
    g_poller = NULL;

    return (g_value == 42 && g_run_loops == 1) ? 0 : 1;
}

int main(void)
{
    if (test_may_sync() != 0) {
        fprintf(stderr, "may_sync failed\n");
        return 1;
    }
    if (test_async_first_order() != 0) {
        fprintf(stderr, "async_first failed\n");
        return 1;
    }
    if (test_cross_thread() != 0) {
        fprintf(stderr, "cross_thread failed\n");
        return 1;
    }
    if (test_poller_run_api() != 0) {
        fprintf(stderr, "poller_run failed\n");
        return 1;
    }
    printf("test_poller_async ok\n");
    return 0;
}
