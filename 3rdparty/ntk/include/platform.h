#ifndef NTK_PLATFORM_H
#define NTK_PLATFORM_H

#include "ntk_export.h"
#include "ntk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ntk_platform_info {
    const char *name; /* "linux", "win32", ... */
    int poller_epoll;
    int poller_iocp; /* 预留 */
} ntk_platform_info_t;

NTK_API void ntk_platform_init(void);
NTK_API void ntk_platform_fini(void);
NTK_API void ntk_platform_get_info(ntk_platform_info_t *out);

/** 单调时钟，毫秒 */
NTK_API uint64_t ntk_monotonic_ms(void);

NTK_API void ntk_sleep_ms(unsigned int ms);

/** 当前线程 ID（仅用于日志/调试，不可跨平台比较语义） */
NTK_API uint64_t ntk_thread_self_id(void);

#ifdef __cplusplus
}
#endif

#endif /* NTK_PLATFORM_H */
