#ifndef ZTK_PLATFORM_H
#define ZTK_PLATFORM_H

#include "ztk_export.h"
#include "ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ztk_platform_info {
    const char *name; /* "linux", "win32", ... */
    int poller_epoll;
    int poller_iocp; /* 预留 */
} ztk_platform_info_t;

ZTK_API void ztk_platform_init(void);
ZTK_API void ztk_platform_fini(void);
ZTK_API void ztk_platform_get_info(ztk_platform_info_t *out);

/** 单调时钟，毫秒 */
ZTK_API uint64_t ztk_monotonic_ms(void);

ZTK_API void ztk_sleep_ms(unsigned int ms);

/** 当前线程 ID（仅用于日志/调试，不可跨平台比较语义） */
ZTK_API uint64_t ztk_thread_self_id(void);

#ifdef __cplusplus
}
#endif

#endif /* ZTK_PLATFORM_H */
