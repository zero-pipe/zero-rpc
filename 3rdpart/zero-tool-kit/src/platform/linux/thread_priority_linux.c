#include "ztk/thread/thread.h"
#include <pthread.h>
#include <sched.h>

void ztk_thread_set_current_priority(ztk_thread_priority_t priority)
{
    static int min_prio = -1;
    static int max_prio = -1;
    if (min_prio < 0) {
        min_prio = sched_get_priority_min(SCHED_FIFO);
        max_prio = sched_get_priority_max(SCHED_FIFO);
        if (min_prio < 0 || max_prio < 0)
            return;
    }
    int p0 = min_prio;
    int p1 = min_prio + (max_prio - min_prio) / 4;
    int p2 = min_prio + (max_prio - min_prio) / 2;
    int p3 = min_prio + (max_prio - min_prio) * 3 / 4;
    int p4 = max_prio;
    int prios[5] = { p0, p1, p2, p3, p4 };
    int idx = (int)priority;
    if (idx < 0)
        idx = 0;
    if (idx > 4)
        idx = 4;

    struct sched_param params;
    params.sched_priority = prios[idx];
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &params);
}
