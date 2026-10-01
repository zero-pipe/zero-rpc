#include "ztk/util/buf.h"
#include "ztk/poller/poller.h"
#include "ztk/thread/thread.h"
#include "ztk/platform.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static volatile int g_stress_done;
static volatile int g_stress_err;

static void stress_worker(void *user)
{
    ztk_buf *b = (ztk_buf *)user;
    int i;

    for (i = 0; i < 20000; ++i) {
        ztk_buf_ref(b);
        ztk_buf_unref(b);
    }
    g_stress_done++;
}

static int test_ref_1000(void)
{
    ztk_buf *b = ztk_buf_alloc(64);
    int i;

    if (!b)
        return 1;

    for (i = 0; i < 1000; ++i)
        ztk_buf_ref(b);

    if (ztk_buf_refcnt(b) != 1001)
        return 1;

    for (i = 0; i < 1000; ++i)
        ztk_buf_unref(b);

    if (ztk_buf_refcnt(b) != 1) {
        ztk_buf_unref(b);
        return 1;
    }

    memcpy((void *)ztk_buf_data(b), "payload", 7);
    ztk_buf_set_len(b, 7);
    if (ztk_buf_len(b) != 7 || memcmp(ztk_buf_data(b), "payload", 7) != 0) {
        ztk_buf_unref(b);
        return 1;
    }

    ztk_buf_unref(b);
    return 0;
}

static int test_cross_thread_stress(void)
{
    ztk_buf *b = ztk_buf_alloc(16);
    ztk_thread *t0;
    ztk_thread *t1;

    if (!b)
        return 1;

    g_stress_done = 0;
    g_stress_err = 0;
    t0 = ztk_thread_create(stress_worker, b);
    t1 = ztk_thread_create(stress_worker, b);
    if (!t0 || !t1) {
        g_stress_err = 1;
        goto done;
    }

    for (int wait = 0; wait < 500 && g_stress_done < 2; ++wait)
        ztk_sleep_ms(10);

    if (g_stress_done < 2)
        g_stress_err = 1;

    ztk_thread_join(t0);
    ztk_thread_join(t1);
    ztk_thread_destroy(t0);
    ztk_thread_destroy(t1);

done:
    if (g_stress_err || ztk_buf_refcnt(b) != 1) {
        ztk_buf_unref(b);
        return 1;
    }
    ztk_buf_unref(b);
    return 0;
}

static int test_pool_buckets_and_lockless(void)
{
    ztk_buf_pool_opts opt;
    ztk_buf_pool *safe;
    ztk_buf_pool *local;
    size_t cap = 0;
    void *p64;
    void *p65;
    void *a;
    void *b;

    memset(&opt, 0, sizeof(opt));
    opt.max_per_bucket = 8;
    opt.thread_safe = 1;
    safe = ztk_buf_pool_create(&opt);
    if (!safe) {
        return 1;
    }

    p64 = ztk_buf_pool_acquire(safe, 65536, &cap);
    if (!p64 || cap != 65536) {
        ztk_buf_pool_release(safe, p64, cap);
        ztk_buf_pool_destroy(safe);
        return 1;
    }
    ztk_buf_pool_release(safe, p64, cap);

    /* 65537 → 下一档 128K */
    p65 = ztk_buf_pool_acquire(safe, 65537, &cap);
    if (!p65 || cap != 131072) {
        ztk_buf_pool_release(safe, p65, cap);
        ztk_buf_pool_destroy(safe);
        return 1;
    }
    ztk_buf_pool_release(safe, p65, cap);
    ztk_buf_pool_destroy(safe);

    opt.thread_safe = 0;
    local = ztk_buf_pool_create(&opt);
    if (!local) {
        return 1;
    }
    a = ztk_buf_pool_acquire(local, 100, &cap);
    if (!a || cap != 512) {
        ztk_buf_pool_release(local, a, cap);
        ztk_buf_pool_destroy(local);
        return 1;
    }
    ztk_buf_pool_release(local, a, cap);
    b = ztk_buf_pool_acquire(local, 100, &cap);
    if (!b || b != a || cap != 512) {
        /* 应复用刚归还的 512 槽 */
        ztk_buf_pool_release(local, b, cap);
        ztk_buf_pool_destroy(local);
        return 1;
    }
    ztk_buf_pool_release(local, b, cap);
    ztk_buf_pool_destroy(local);
    return 0;
}

static volatile ztk_buf *g_defer_buf;

static void defer_unref_worker(void *user)
{
    (void)user;
    ztk_buf_unref((ztk_buf *)g_defer_buf);
    g_defer_buf = NULL;
}

static int test_lockless_cross_thread_unref(void)
{
    ztk_poller *pol;
    ztk_buf_pool_opts opt;
    ztk_buf_pool *local;
    ztk_buf *b;
    ztk_buf *b2;
    const void *first;
    ztk_thread *t;
    int i;

    pol = ztk_poller_create();
    if (!pol) {
        return 1;
    }

    memset(&opt, 0, sizeof(opt));
    opt.max_per_bucket = 8;
    opt.thread_safe = 0;
    local = ztk_buf_pool_create(&opt);
    if (!local) {
        ztk_poller_destroy(pol);
        return 1;
    }
    ztk_poller_attach_buf_pool(pol, local);
    ztk_poller_bind_thread(pol);

    b = ztk_buf_alloc_local(pol, 100);
    if (!b) {
        ztk_poller_attach_buf_pool(pol, NULL);
        ztk_buf_pool_destroy(local);
        ztk_poller_destroy(pol);
        return 1;
    }
    first = ztk_buf_data(b);
    g_defer_buf = b;

    t = ztk_thread_create(defer_unref_worker, NULL);
    if (!t) {
        ztk_buf_unref(b);
        ztk_poller_attach_buf_pool(pol, NULL);
        ztk_buf_pool_destroy(local);
        ztk_poller_destroy(pol);
        return 1;
    }

    for (i = 0; i < 200 && g_defer_buf; ++i) {
        ztk_poller_poll(pol, 5);
        ztk_sleep_ms(1);
    }
    ztk_thread_join(t);
    ztk_thread_destroy(t);

    /* 排空可能仍在队列里的 release 任务 */
    for (i = 0; i < 20; ++i) {
        ztk_poller_poll(pol, 5);
    }

    b2 = ztk_buf_alloc_local(pol, 100);
    if (!b2 || ztk_buf_data(b2) != first) {
        /* 跨线程 unref 应回投 owner 入池，再次 alloc 复用同一块 */
        if (b2) {
            ztk_buf_unref(b2);
        }
        ztk_poller_unbind_thread(pol);
        ztk_poller_attach_buf_pool(pol, NULL);
        ztk_buf_pool_destroy(local);
        ztk_poller_destroy(pol);
        return 1;
    }
    ztk_buf_unref(b2);

    ztk_poller_unbind_thread(pol);
    ztk_poller_attach_buf_pool(pol, NULL);
    ztk_buf_pool_destroy(local);
    ztk_poller_destroy(pol);
    return 0;
}

static int test_freelist_and_stats(void)
{
    ztk_buf_pool_opts opt;
    ztk_buf_pool *pool;
    void *ptrs[200];
    size_t cap = 0;
    ztk_buf_pool_stats st;
    unsigned counts[9];
    int i;
    void *big;

    memset(&opt, 0, sizeof(opt));
    opt.max_per_bucket = 200; /* 超过旧 SLOT_STRUCT_MAX=128，验证 freelist 可扩 */
    opt.thread_safe = 1;
    pool = ztk_buf_pool_create(&opt);
    if (!pool) {
        return 1;
    }

    for (i = 0; i < 200; ++i) {
        ptrs[i] = ztk_buf_pool_acquire(pool, 100, &cap);
        if (!ptrs[i] || cap != 512) {
            while (--i >= 0) {
                ztk_buf_pool_release(pool, ptrs[i], 512);
            }
            ztk_buf_pool_destroy(pool);
            return 1;
        }
    }
    ztk_buf_pool_get_stats(pool, &st);
    if (st.acquire_miss != 200 || st.acquire_hit != 0) {
        ztk_buf_pool_destroy(pool);
        return 1;
    }

    for (i = 0; i < 200; ++i) {
        ztk_buf_pool_release(pool, ptrs[i], 512);
    }
    ztk_buf_pool_get_stats(pool, &st);
    if (st.release_cached != 200 || ztk_buf_pool_bucket_counts(pool, counts, 9) < 1 ||
        counts[0] != 200) {
        ztk_buf_pool_destroy(pool);
        return 1;
    }

    for (i = 0; i < 50; ++i) {
        ptrs[i] = ztk_buf_pool_acquire(pool, 100, &cap);
        if (!ptrs[i]) {
            ztk_buf_pool_destroy(pool);
            return 1;
        }
    }
    ztk_buf_pool_get_stats(pool, &st);
    if (st.acquire_hit < 50) {
        ztk_buf_pool_destroy(pool);
        return 1;
    }
    for (i = 0; i < 50; ++i) {
        ztk_buf_pool_release(pool, ptrs[i], 512);
    }

    big = ztk_buf_pool_acquire(pool, 3 * 1024 * 1024, &cap);
    if (!big || cap != 3 * 1024 * 1024) {
        free(big);
        ztk_buf_pool_destroy(pool);
        return 1;
    }
    ztk_buf_pool_release(pool, big, cap);
    ztk_buf_pool_get_stats(pool, &st);
    if (st.acquire_oversize < 1 || st.release_dropped < 1) {
        ztk_buf_pool_destroy(pool);
        return 1;
    }

    ztk_buf_pool_destroy(pool);
    return 0;
}

int main(void)
{
    if (test_ref_1000() != 0) {
        fprintf(stderr, "ref 1000 failed\n");
        return 1;
    }
    if (test_cross_thread_stress() != 0) {
        fprintf(stderr, "cross-thread stress failed\n");
        return 1;
    }
    if (test_pool_buckets_and_lockless() != 0) {
        fprintf(stderr, "pool buckets/lockless failed\n");
        return 1;
    }
    if (test_lockless_cross_thread_unref() != 0) {
        fprintf(stderr, "lockless cross-thread unref failed\n");
        return 1;
    }
    if (test_freelist_and_stats() != 0) {
        fprintf(stderr, "freelist/stats failed\n");
        return 1;
    }
    printf("test_buf ok\n");
    return 0;
}
