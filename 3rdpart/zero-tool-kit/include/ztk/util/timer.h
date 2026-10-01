#ifndef ZTK_UTIL_TIMER_H
#define ZTK_UTIL_TIMER_H

#include "../ztk_export.h"
#include "../ztk_errno.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ztk_poller;
typedef struct ztk_poller ztk_poller;
typedef struct ztk_poller_timer ztk_poller_timer;
typedef struct ztk_timer ztk_timer;

/**
 * 延迟任务回调：返回下次间隔（毫秒），0 表示结束。
 */
typedef uint64_t (*ztk_poller_delay_cb)(void *user);

/** 在 poller 线程触发；跨线程投递用 async 入队 */
ZTK_API ztk_poller_timer *ztk_poller_do_delay(ztk_poller *p, uint64_t delay_ms,
                                                 ztk_poller_delay_cb cb, void *user);

ZTK_API void ztk_poller_timer_cancel(ztk_poller_timer *timer);

/** 固定间隔定时器（repeat=0 表示单次） */
typedef void (*ztk_timer_cb)(void *user);

ZTK_API ztk_timer *ztk_timer_start(ztk_poller *p, uint64_t interval_ms, int repeat,
                                      ztk_timer_cb cb, void *user);
ZTK_API void ztk_timer_stop(ztk_timer *timer);

/** Wall-clock milliseconds since Unix epoch (shared with ZMS play/RTCP clocks). */
ZTK_API uint64_t ztk_wall_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* ZTK_UTIL_TIMER_H */
