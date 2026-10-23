#include "platform.h"
#include "ntk_config.h"
#include <pthread.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static ntk_platform_info_t s_info = {
    .name = "linux",
    .poller_epoll = 0,
    .poller_iocp = 0
};

void ntk_platform_init(void)
{
#if defined(NTK_HAVE_EPOLL)
    s_info.poller_epoll = 1;
#endif
}

void ntk_platform_fini(void)
{
}

void ntk_platform_get_info(ntk_platform_info_t *out)
{
    if (!out)
        return;
    *out = s_info;
}

uint64_t ntk_monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void ntk_sleep_ms(unsigned int ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    nanosleep(&ts, NULL);
}

uint64_t ntk_thread_self_id(void)
{
#if defined(SYS_gettid)
    return (uint64_t)syscall(SYS_gettid);
#else
    return (uint64_t)(uintptr_t)pthread_self();
#endif
}
