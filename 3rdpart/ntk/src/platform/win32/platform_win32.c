#include "platform.h"
#include "ntk_config.h"
#include "../../net/internal/sock_platform.h"
#include <stdint.h>
#include <windows.h>

static ntk_platform_info_t s_info = {
    .name = "win32",
    .poller_epoll = 0,
    .poller_iocp = 0
};

void ntk_platform_init(void)
{
    if (ntk_sockplat_init() == NTK_OK) {
#if defined(NTK_HAVE_WEPOLL)
        s_info.poller_epoll = 1;
#endif
    }
}

void ntk_platform_fini(void)
{
    ntk_sockplat_fini();
}

void ntk_platform_get_info(ntk_platform_info_t *out)
{
    if (out)
        *out = s_info;
}

uint64_t ntk_monotonic_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart * 1000 / freq.QuadPart);
}

void ntk_sleep_ms(unsigned int ms)
{
    Sleep(ms);
}

uint64_t ntk_thread_self_id(void)
{
    return (uint64_t)GetCurrentThreadId();
}
