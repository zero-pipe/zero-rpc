#ifndef NTK_POLLER_H
#define NTK_POLLER_H

#include "../ntk_export.h"
#include "../ntk_errno.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ntk_poller ntk_poller;

enum {
    NTK_POLL_IN = 1 << 0,
    NTK_POLL_OUT = 1 << 1,
    NTK_POLL_ERR = 1 << 2,
    NTK_POLL_HUP = 1 << 3
};

/** poller 负载统计滑动窗口 (ms) */
#define NTK_POLLER_LOAD_WINDOW_MS  2000
/**
 * ntk_poller_run 默认最大 sleep (ms)。
 * 流媒体 10ms 级 timer 需要 ≤10；见 docs/10-timer-scheduler-contract.md。
 */
#define NTK_POLLER_PENDING_POLL_MS 10

typedef void (*ntk_poller_cb)(int fd, unsigned events, void *user);

/** 在 poller 线程内执行的任务（勿阻塞） */
typedef void (*ntk_poller_task_fn)(void *user);

NTK_API ntk_poller *ntk_poller_create(void);
NTK_API void ntk_poller_destroy(ntk_poller *p);

NTK_API ntk_err_t ntk_poller_add(ntk_poller *p, int fd, unsigned events, ntk_poller_cb cb, void *user);
NTK_API ntk_err_t ntk_poller_mod(ntk_poller *p, int fd, unsigned events);
NTK_API ntk_err_t ntk_poller_del(ntk_poller *p, int fd);

/**
 * @param timeout_ms -1 表示无限等待，0 表示立即返回
 * @return 就绪事件数，0 表示超时，<0 错误
 */
NTK_API int ntk_poller_poll(ntk_poller *p, int timeout_ms);

NTK_API int ntk_poller_wake_read_fd(ntk_poller *p);
NTK_API int ntk_poller_wake_write_fd(ntk_poller *p);
NTK_API ntk_err_t ntk_poller_wake(ntk_poller *p);

/**
 * 投递任务到 poller 线程（pipe 唤醒 + 队列）。
 * @param may_sync 非 0 且当前已是 poller 线程则立即执行
 * @note user 须在任务执行完成前保持有效（勿传栈上局部变量地址）
 */
NTK_API ntk_err_t ntk_poller_async(ntk_poller *p, ntk_poller_task_fn fn, void *user, int may_sync);
NTK_API ntk_err_t ntk_poller_async_first(ntk_poller *p, ntk_poller_task_fn fn, void *user, int may_sync);

/** 不 poll 时手动排空任务队列 */
NTK_API void ntk_poller_process_pending(ntk_poller *p);

/** 标记当前线程为该 poller 的事件线程（在 run 前也可手动调用） */
NTK_API void ntk_poller_bind_thread(ntk_poller *p);
NTK_API void ntk_poller_unbind_thread(ntk_poller *p);
NTK_API int ntk_poller_is_current_thread(ntk_poller *p);

/**
 * 当前线程绑定的 poller（ntk_poller_run / bind_thread 设置）；未绑定返回 NULL。
 * 供 poller 本地资源分配使用，避免在回调里逐处传递 poller 句柄。
 */
NTK_API ntk_poller *ntk_poller_current(void);

/** 阻塞循环 poll，*stop_flag 非 0 时退出 */
NTK_API void ntk_poller_run(ntk_poller *p, volatile int *stop_flag);

/** 线程负载 0–100（仅在 poll/run 路径统计），越高越忙 */
NTK_API int ntk_poller_get_load(ntk_poller *p);

#ifdef __cplusplus
}
#endif

#endif /* NTK_POLLER_H */
