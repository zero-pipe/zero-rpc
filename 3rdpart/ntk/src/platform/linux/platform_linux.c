#include "ztk/platform.h"
#include "ztk_config.h"
#include <pthread.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static ztk_platform_info_t s_info = {
    .name = "linux",
    .poller_epoll = 0,
    .poller_iocp = 0
};

void ztk_platform_init(void)
{
#if defined(ZTK_HAVE_EPOLL)
    s_info.poller_epoll = 1;
#endif
}

void ztk_platform_fini(void)
{
}

void ztk_platform_get_info(ztk_platform_info_t *out)
{
    if (!out)
        return;
    *out = s_info;
}

uint64_t ztk_monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void ztk_sleep_ms(unsigned int ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    nanosleep(&ts, NULL);
}

uint64_t ztk_thread_self_id(void)
{
#if defined(SYS_gettid)
    return (uint64_t)syscall(SYS_gettid);
#else
    return (uint64_t)(uintptr_t)pthread_self();
#endif
}
