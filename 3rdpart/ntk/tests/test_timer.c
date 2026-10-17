#include "ztk/util/timer.h"
#include "ztk/poller/poller.h"
#include <stdio.h>

static volatile int g_ticks;
static volatile int g_done;

static uint64_t on_delay(void *user)
{
    (void)user;
    g_ticks++;
    if (g_ticks >= 3) {
        g_done = 1;
        return 0;
    }
    return 50;
}

static void on_timer(void *user)
{
    (void)user;
    g_ticks++;
}

int main(void)
{
    ztk_poller *p = ztk_poller_create();
    if (!p)
        return 1;

    ztk_poller_bind_thread(p);
    g_ticks = 0;
    g_done = 0;

    if (!ztk_poller_do_delay(p, 30, on_delay, NULL)) {
        ztk_poller_destroy(p);
        return 1;
    }

    for (int i = 0; i < 500 && !g_done; ++i)
        ztk_poller_poll(p, 100);

    if (g_ticks < 3) {
        fprintf(stderr, "delay ticks=%d\n", g_ticks);
        ztk_poller_destroy(p);
        return 1;
    }

    g_ticks = 0;
    ztk_timer *tm = ztk_timer_start(p, 40, 1, on_timer, NULL);
    if (!tm) {
        ztk_poller_destroy(p);
        return 1;
    }

    for (int i = 0; i < 30 && g_ticks < 1; ++i)
        ztk_poller_poll(p, 50);
    ztk_timer_stop(tm);

    if (g_ticks < 1) {
        fprintf(stderr, "timer wrapper failed\n");
        ztk_poller_destroy(p);
        return 1;
    }

    /* ZTK-S3: 10ms repeat timer must fire while poll timeout is 100ms (resolve_timeout caps sleep). */
    g_ticks = 0;
    tm = ztk_timer_start(p, 10, 1, on_timer, NULL);
    if (!tm) {
        ztk_poller_destroy(p);
        return 1;
    }
    for (int i = 0; i < 200 && g_ticks < 1; ++i)
        ztk_poller_poll(p, 100);
    ztk_timer_stop(tm);
    if (g_ticks < 1) {
        fprintf(stderr, "10ms timer under poll(100) failed\n");
        ztk_poller_destroy(p);
        return 1;
    }

    ztk_poller_unbind_thread(p);
    ztk_poller_destroy(p);
    printf("test_timer ok\n");
    return 0;
}
