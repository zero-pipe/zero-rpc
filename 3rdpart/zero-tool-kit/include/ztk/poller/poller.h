#ifndef ZTK_POLLER_H
#define ZTK_POLLER_H

#include "../ztk_export.h"
#include "../ztk_errno.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ztk_poller ztk_poller;

enum {
    ZTK_POLL_IN = 1 << 0,
    ZTK_POLL_OUT = 1 << 1,
    ZTK_POLL_ERR = 1 << 2,
    ZTK_POLL_HUP = 1 << 3
};

/** poller 负载统计滑动窗口 (ms) */
#define ZTK_POLLER_LOAD_WINDOW_MS  2000
/**
 * ztk_poller_run 默认最大 sleep (ms)。
 * 流媒体 10ms 级 timer 需要 ≤10；见 docs/10-timer-scheduler-contract.md。
 */
#define ZTK_POLLER_PENDING_POLL_MS 10

typedef void (*ztk_poller_cb)(int fd, unsigned events, void *user);

/** 在 poller 线程内执行的任务（勿阻塞） */
typedef void (*ztk_poller_task_fn)(void *user);

ZTK_API ztk_poller *ztk_poller_create(void);
ZTK_API void ztk_poller_destroy(ztk_poller *p);

ZTK_API ztk_err_t ztk_poller_add(ztk_poller *p, int fd, unsigned events, ztk_poller_cb cb, void *user);
ZTK_API ztk_err_t ztk_poller_mod(ztk_poller *p, int fd, unsigned events);
ZTK_API ztk_err_t ztk_poller_del(ztk_poller *p, int fd);

/**
 * @param timeout_ms -1 表示无限等待，0 表示立即返回
 * @return 就绪事件数，0 表示超时，<0 错误
 */
ZTK_API int ztk_poller_poll(ztk_poller *p, int timeout_ms);

ZTK_API int ztk_poller_wake_read_fd(ztk_poller *p);
ZTK_API int ztk_poller_wake_write_fd(ztk_poller *p);
ZTK_API ztk_err_t ztk_poller_wake(ztk_poller *p);

/**
 * 投递任务到 poller 线程（pipe 唤醒 + 队列）。
 * @param may_sync 非 0 且当前已是 poller 线程则立即执行
 * @note user 须在任务执行完成前保持有效（勿传栈上局部变量地址）
 */
ZTK_API ztk_err_t ztk_poller_async(ztk_poller *p, ztk_poller_task_fn fn, void *user, int may_sync);
ZTK_API ztk_err_t ztk_poller_async_first(ztk_poller *p, ztk_poller_task_fn fn, void *user, int may_sync);

/** 不 poll 时手动排空任务队列 */
ZTK_API void ztk_poller_process_pending(ztk_poller *p);

/** 标记当前线程为该 poller 的事件线程（在 run 前也可手动调用） */
ZTK_API void ztk_poller_bind_thread(ztk_poller *p);
ZTK_API void ztk_poller_unbind_thread(ztk_poller *p);
ZTK_API int ztk_poller_is_current_thread(ztk_poller *p);

/** 阻塞循环 poll，*stop_flag 非 0 时退出 */
ZTK_API void ztk_poller_run(ztk_poller *p, volatile int *stop_flag);

/** 线程负载 0–100（仅在 poll/run 路径统计），越高越忙 */
ZTK_API int ztk_poller_get_load(ztk_poller *p);

#ifdef __cplusplus
}
#endif

#endif /* ZTK_POLLER_H */
