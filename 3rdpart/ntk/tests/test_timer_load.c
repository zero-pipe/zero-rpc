#include "ztk/util/timer.h"
#include "ztk/poller/poller.h"
#include "ztk/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOAD_TIMER_COUNT 100
#define LOAD_INTERVAL_MS 10
#define LOAD_RUN_MS      2000

static int load_is_wsl(void)
{
#if defined(__linux__)
    FILE *f = fopen("/proc/version", "r");
    char buf[256];
    if (!f)
        return 0;
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return strstr(buf, "Microsoft") != NULL || strstr(buf, "microsoft") != NULL;
#else
    return 0;
#endif
}

static unsigned load_max_drift_ms(void)
{
#if defined(_WIN32)
    return 20u;
#else
    return load_is_wsl() ? 20u : 1u;
#endif
}

typedef struct load_timer_ctx {
    volatile int fires;
    uint64_t last_fire_ms;
    uint64_t max_drift_ms;
} load_timer_ctx;

static load_timer_ctx g_ctx[LOAD_TIMER_COUNT];
static ztk_timer *g_timers[LOAD_TIMER_COUNT];

static void on_load_timer(void *user)
{
    load_timer_ctx *c = (load_timer_ctx *)user;
    uint64_t now = ztk_monotonic_ms();
    uint64_t drift;

    if (c->last_fire_ms) {
        drift = now - c->last_fire_ms;
        if (drift > LOAD_INTERVAL_MS + c->max_drift_ms)
            c->max_drift_ms = drift - LOAD_INTERVAL_MS;
    }
    c->last_fire_ms = now;
    ++c->fires;
}

int main(void)
{
    ztk_poller *p = ztk_poller_create();
    int i;
    uint64_t t0;
    uint64_t max_drift = 0;
    int min_fires = 0;

    if (!p)
        return 1;

    ztk_poller_bind_thread(p);
    for (i = 0; i < LOAD_TIMER_COUNT; ++i) {
        g_ctx[i].fires = 0;
        g_ctx[i].last_fire_ms = 0;
        g_ctx[i].max_drift_ms = 0;
        g_timers[i] = ztk_timer_start(p, LOAD_INTERVAL_MS, 1, on_load_timer, &g_ctx[i]);
        if (!g_timers[i]) {
            fprintf(stderr, "timer_start %d failed\n", i);
            return 1;
        }
    }

    t0 = ztk_monotonic_ms();
    while (ztk_monotonic_ms() - t0 < LOAD_RUN_MS)
        ztk_poller_poll(p, (int)ZTK_POLLER_PENDING_POLL_MS);

    min_fires = g_ctx[0].fires;
    for (i = 0; i < LOAD_TIMER_COUNT; ++i) {
        ztk_timer_stop(g_timers[i]);
        if (g_ctx[i].max_drift_ms > max_drift)
            max_drift = g_ctx[i].max_drift_ms;
        if (g_ctx[i].fires < min_fires)
            min_fires = g_ctx[i].fires;
    }

    if (min_fires < (int)(LOAD_RUN_MS / LOAD_INTERVAL_MS / 2)) {
        fprintf(stderr, "too few fires min=%d\n", min_fires);
        return 1;
    }
    if (max_drift > load_max_drift_ms()) {
        fprintf(stderr, "max drift %llu ms (limit %u)\n",
            (unsigned long long)max_drift, load_max_drift_ms());
        return 1;
    }

    ztk_poller_unbind_thread(p);
    ztk_poller_destroy(p);
    printf("test_timer_load ok (max_drift=%llu ms)\n", (unsigned long long)max_drift);
    return 0;
}
