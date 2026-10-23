#ifndef NTK_THREAD_THREAD_H
#define NTK_THREAD_THREAD_H

#include "../ntk_export.h"
#include "../ntk_errno.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ntk_thread ntk_thread;

typedef void (*ntk_thread_fn)(void *user);

/** 线程优先级枚举，LOWEST=0 … HIGHEST=4 */
typedef enum ntk_thread_priority {
    NTK_THREAD_PRIO_LOWEST = 0,
    NTK_THREAD_PRIO_LOW = 1,
    NTK_THREAD_PRIO_NORMAL = 2,
    NTK_THREAD_PRIO_HIGH = 3,
    NTK_THREAD_PRIO_HIGHEST = 4
} ntk_thread_priority_t;

NTK_API ntk_thread *ntk_thread_create(ntk_thread_fn fn, void *user);
NTK_API ntk_thread *ntk_thread_create_ex(ntk_thread_fn fn, void *user, ntk_thread_priority_t priority);
NTK_API ntk_err_t ntk_thread_join(ntk_thread *t);
NTK_API void ntk_thread_destroy(ntk_thread *t);

/** 设置当前线程调度优先级（新建线程在入口回调前也会应用 create_ex 的 priority） */
NTK_API void ntk_thread_set_current_priority(ntk_thread_priority_t priority);

/** CPU 逻辑核数，至少为 1 */
NTK_API unsigned int ntk_thread_hardware_concurrency(void);

#ifdef __cplusplus
}
#endif

#endif /* NTK_THREAD_THREAD_H */
