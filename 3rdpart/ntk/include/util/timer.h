#ifndef NTK_UTIL_TIMER_H
#define NTK_UTIL_TIMER_H

#include "../ntk_export.h"
#include "../ntk_errno.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ntk_poller;
typedef struct ntk_poller ntk_poller;
typedef struct ntk_poller_timer ntk_poller_timer;
typedef struct ntk_timer ntk_timer;

/**
 * 延迟任务回调：返回下次间隔（毫秒），0 表示结束。
 */
typedef uint64_t (*ntk_poller_delay_cb)(void *user);

/** 在 poller 线程触发；跨线程投递用 async 入队 */
NTK_API ntk_poller_timer *ntk_poller_do_delay(ntk_poller *p, uint64_t delay_ms,
                                                 ntk_poller_delay_cb cb, void *user);

NTK_API void ntk_poller_timer_cancel(ntk_poller_timer *timer);

/** 固定间隔定时器（repeat=0 表示单次） */
typedef void (*ntk_timer_cb)(void *user);

NTK_API ntk_timer *ntk_timer_start(ntk_poller *p, uint64_t interval_ms, int repeat,
                                      ntk_timer_cb cb, void *user);
NTK_API void ntk_timer_stop(ntk_timer *timer);

/** Wall-clock milliseconds since Unix epoch (shared with ZMS play/RTCP clocks). */
NTK_API uint64_t ntk_wall_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* NTK_UTIL_TIMER_H */
