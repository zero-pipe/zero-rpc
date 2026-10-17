#include "ztk/platform.h"
#include "ztk/poller/poller.h"
#include "ztk/poller/poller_pool.h"
#include "ztk/thread/thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int g_done;
static int g_target_load;

static void bump_load(void *user)
{
    int ms = *(int *)user;
    uint64_t end = ztk_monotonic_ms() + (uint64_t)ms;
    while (ztk_monotonic_ms() < end) {}
    g_done = 1;
}

static int test_pool_get_prefers_light(void)
{
    ztk_poller_pool_opts_t opts = { .size = 2, .prefer_current_thread = 0,
        .thread_priority = ZTK_THREAD_PRIO_NORMAL };
    ztk_poller_pool *pool = ztk_poller_pool_create(&opts);
    if (!pool)
        return 1;

    ztk_poller *p0 = ztk_poller_pool_at(pool, 0);
    ztk_poller *p1 = ztk_poller_pool_at(pool, 1);
    if (!p0 || !p1)
        goto fail;

    g_target_load = 80;
    g_done = 0;
    if (ztk_poller_async(p0, bump_load, &g_target_load, 0) != ZTK_OK)
        goto fail;

    if (ztk_poller_pool_start(pool) != ZTK_OK)
        goto fail;

    for (int i = 0; i < 500 && !g_done; ++i)
        ztk_sleep_ms(10);

    if (!g_done)
        goto fail_stop;

    ztk_poller *picked = ztk_poller_pool_get(pool, 0);
    if (!picked || picked == p0)
        goto fail_stop;

    ztk_poller_pool_stop(pool);
    ztk_poller_pool_destroy(pool);
    return 0;

fail_stop:
    ztk_poller_pool_stop(pool);
fail:
    ztk_poller_pool_destroy(pool);
    return 1;
}

static int test_pinning_by_key(void)
{
    ztk_poller_pool_opts_t opts = { .size = 4, .thread_priority = ZTK_THREAD_PRIO_NORMAL };
    ztk_poller_pool *pool = ztk_poller_pool_create(&opts);
    ztk_poller *first;
    int i;

    if (!pool)
        return 1;

    first = ztk_poller_pool_get_by_key(pool, 0x12345678ULL);
    if (!first)
        goto fail;

    for (i = 0; i < 100; ++i) {
        if (ztk_poller_pool_get_by_key(pool, 0x12345678ULL) != first)
            goto fail;
    }

    ztk_poller_pool_destroy(pool);
    return 0;
fail:
    ztk_poller_pool_destroy(pool);
    return 1;
}

static int test_tcp_pick_helper(void)
{
    ztk_poller_pool_opts_t opts = { .size = 2, .thread_priority = ZTK_THREAD_PRIO_NORMAL };
    ztk_poller_pool *pool = ztk_poller_pool_create(&opts);
    if (!pool)
        return 1;

    ztk_poller *a = ztk_poller_pool_at(pool, 0);
    ztk_poller *b = ztk_tcp_pick_poller_from_pool(pool, a);
    if (!b)
        goto fail;

    ztk_poller_pool_destroy(pool);
    return 0;
fail:
    ztk_poller_pool_destroy(pool);
    return 1;
}

int main(void)
{
    if (test_pinning_by_key() != 0) {
        fprintf(stderr, "pinning by key failed\n");
        return 1;
    }
    if (test_tcp_pick_helper() != 0) {
        fprintf(stderr, "tcp pick from pool failed\n");
        return 1;
    }
    if (test_pool_get_prefers_light() != 0) {
        fprintf(stderr, "pool load balance failed\n");
        return 1;
    }
    printf("test_poller_pool ok\n");
    return 0;
}
