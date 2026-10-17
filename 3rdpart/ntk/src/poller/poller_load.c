#include "poller_internal.h"
#include "poller/poller.h"
#include "platform.h"
#include <stdlib.h>

void ntk_poller_load_init(ntk_poller *p)
{
    if (!p)
        return;
    p->load.mtx = ntk_mutex_create(NTK_MUTEX_NORMAL);
    p->load.last_ms = ntk_monotonic_ms();
    p->load.run_ms = 0;
    p->load.sleep_ms = 0;
    p->load.sleeping = 0;
}

void ntk_poller_load_fini(ntk_poller *p)
{
    if (!p || !p->load.mtx)
        return;
    ntk_mutex_destroy(p->load.mtx);
    p->load.mtx = NULL;
}

static void mark_time(ntk_poller_load_state *st, int sleeping)
{
    uint64_t now = ntk_monotonic_ms();
    uint64_t delta = now - st->last_ms;
    st->last_ms = now;
    if (st->sleeping)
        st->sleep_ms += delta;
    else
        st->run_ms += delta;
    st->sleeping = sleeping;

    /* 滑动窗口：各保留最近 2s 量级 */
    if (st->run_ms + st->sleep_ms > NTK_POLLER_LOAD_WINDOW_MS) {
        st->run_ms /= 2;
        st->sleep_ms /= 2;
    }
}

void ntk_poller_load_on_sleep(ntk_poller *p)
{
    if (!p || !p->load.mtx)
        return;
    ntk_mutex_lock(p->load.mtx);
    if (!p->load.sleeping)
        mark_time(&p->load, 1);
    ntk_mutex_unlock(p->load.mtx);
}

void ntk_poller_load_on_wake(ntk_poller *p)
{
    if (!p || !p->load.mtx)
        return;
    ntk_mutex_lock(p->load.mtx);
    if (p->load.sleeping)
        mark_time(&p->load, 0);
    ntk_mutex_unlock(p->load.mtx);
}

int ntk_poller_load_percent(ntk_poller *p)
{
    if (!p || !p->load.mtx)
        return 0;
    ntk_mutex_lock(p->load.mtx);
    if (p->load.sleeping) {
        uint64_t now = ntk_monotonic_ms();
        p->load.sleep_ms += now - p->load.last_ms;
        p->load.last_ms = now;
    } else {
        uint64_t now = ntk_monotonic_ms();
        p->load.run_ms += now - p->load.last_ms;
        p->load.last_ms = now;
    }
    uint64_t total = p->load.run_ms + p->load.sleep_ms;
    int pct = 0;
    if (total > 0)
        pct = (int)((p->load.run_ms * 100) / total);
    ntk_mutex_unlock(p->load.mtx);
    if (pct > 100)
        pct = 100;
    return pct;
}

int ntk_poller_get_load(ntk_poller *p)
{
    return ntk_poller_load_percent(p);
}
