#ifndef ZTK_THREAD_THREAD_H
#define ZTK_THREAD_THREAD_H

#include "../ztk_export.h"
#include "../ztk_errno.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ztk_thread ztk_thread;

typedef void (*ztk_thread_fn)(void *user);

/** 线程优先级枚举，LOWEST=0 … HIGHEST=4 */
typedef enum ztk_thread_priority {
    ZTK_THREAD_PRIO_LOWEST = 0,
    ZTK_THREAD_PRIO_LOW = 1,
    ZTK_THREAD_PRIO_NORMAL = 2,
    ZTK_THREAD_PRIO_HIGH = 3,
    ZTK_THREAD_PRIO_HIGHEST = 4
} ztk_thread_priority_t;

ZTK_API ztk_thread *ztk_thread_create(ztk_thread_fn fn, void *user);
ZTK_API ztk_thread *ztk_thread_create_ex(ztk_thread_fn fn, void *user, ztk_thread_priority_t priority);
ZTK_API ztk_err_t ztk_thread_join(ztk_thread *t);
ZTK_API void ztk_thread_destroy(ztk_thread *t);

/** 设置当前线程调度优先级（新建线程在入口回调前也会应用 create_ex 的 priority） */
ZTK_API void ztk_thread_set_current_priority(ztk_thread_priority_t priority);

/** CPU 逻辑核数，至少为 1 */
ZTK_API unsigned int ztk_thread_hardware_concurrency(void);

#ifdef __cplusplus
}
#endif

#endif /* ZTK_THREAD_THREAD_H */
