#include "thread/thread.h"
#include <windows.h>

void ntk_thread_set_current_priority(ntk_thread_priority_t priority)
{
    static const int win_prio[5] = {
        THREAD_PRIORITY_LOWEST,
        THREAD_PRIORITY_BELOW_NORMAL,
        THREAD_PRIORITY_NORMAL,
        THREAD_PRIORITY_ABOVE_NORMAL,
        THREAD_PRIORITY_HIGHEST
    };
    int idx = (int)priority;
    if (idx < 0)
        idx = 0;
    if (idx > 4)
        idx = 4;
    if (priority != NTK_THREAD_PRIO_NORMAL)
        SetThreadPriority(GetCurrentThread(), win_prio[idx]);
}
