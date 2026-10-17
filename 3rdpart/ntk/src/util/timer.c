#include "util/timer.h"
#include "poller/poller.h"
#include <stdlib.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/time.h>
#endif

struct ntk_timer {
    ntk_poller *poller;
    ntk_poller_timer *handle;
    uint64_t interval_ms;
    int repeat;
    ntk_timer_cb cb;
    void *user;
};

static uint64_t timer_bridge(void *user)
{
    ntk_timer *t = (ntk_timer *)user;
    if (t->cb)
        t->cb(t->user);
    return t->repeat ? t->interval_ms : 0;
}

ntk_timer *ntk_timer_start(ntk_poller *p, uint64_t interval_ms, int repeat, ntk_timer_cb cb, void *user)
{
    if (!p || !cb || interval_ms == 0)
        return NULL;

    ntk_timer *t = (ntk_timer *)calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->poller = p;
    t->interval_ms = interval_ms;
    t->repeat = repeat ? 1 : 0;
    t->cb = cb;
    t->user = user;

    t->handle = ntk_poller_do_delay(p, interval_ms, timer_bridge, t);
    if (!t->handle) {
        free(t);
        return NULL;
    }
    return t;
}

void ntk_timer_stop(ntk_timer *t)
{
    if (!t)
        return;
    if (t->handle)
        ntk_poller_timer_cancel(t->handle);
    free(t);
}

uint64_t ntk_wall_ms(void)
{
#if defined(_WIN32)
    FILETIME ft;
    ULARGE_INTEGER u;

    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart / 10000ULL - 11644473600000ULL;
#else
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
#endif
}
