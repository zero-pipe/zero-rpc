#include "ztk/platform.h"
#include "ztk_config.h"
#include "../../net/internal/sock_platform.h"
#include <stdint.h>
#include <windows.h>

static ztk_platform_info_t s_info = {
    .name = "win32",
    .poller_epoll = 0,
    .poller_iocp = 0
};

void ztk_platform_init(void)
{
    if (ztk_sockplat_init() == ZTK_OK) {
#if defined(ZTK_HAVE_WEPOLL)
        s_info.poller_epoll = 1;
#endif
    }
}

void ztk_platform_fini(void)
{
    ztk_sockplat_fini();
}

void ztk_platform_get_info(ztk_platform_info_t *out)
{
    if (out)
        *out = s_info;
}

uint64_t ztk_monotonic_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart * 1000 / freq.QuadPart);
}

void ztk_sleep_ms(unsigned int ms)
{
    Sleep(ms);
}

uint64_t ztk_thread_self_id(void)
{
    return (uint64_t)GetCurrentThreadId();
}
