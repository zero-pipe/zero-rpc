#ifndef ZTK_POLLER_INTERNAL_H
#define ZTK_POLLER_INTERNAL_H

#include "ztk/thread/sync.h"
#include <stddef.h>
#include <stdint.h>

typedef struct ztk_poller ztk_poller;

typedef void (*ztk_poller_task_fn)(void *user);
typedef uint64_t (*ztk_poller_delay_cb)(void *user);

typedef struct ztk_poller_task {
    ztk_poller_task_fn fn;
    void *user;
    struct ztk_poller_task *next;
} ztk_poller_task;

struct ztk_poller_timer {
    ztk_poller *poller;
    ztk_poller_delay_cb cb;
    void *user;
    int cancelled;
    unsigned refs;
    uint64_t deadline_ms;
    size_t heap_index;
};

typedef struct ztk_poller_timer ztk_poller_timer;

typedef struct ztk_fd_entry ztk_fd_entry;

typedef struct ztk_poller_load_state {
    ztk_mutex *mtx;
    uint64_t last_ms;
    uint64_t run_ms;
    uint64_t sleep_ms;
    int sleeping;
} ztk_poller_load_state;

struct ztk_buf_pool;

struct ztk_poller {
    intptr_t epfd;
    int wake_r;
    int wake_w;
    ztk_fd_entry *entries;

    /** 可选：per-poller 字节池（attach 挂载；poller 不拥有生命周期；建议 thread_safe=0） */
    struct ztk_buf_pool *buf_pool;

    ztk_mutex *task_mtx;
    ztk_poller_task *task_head;
    ztk_poller_task *task_tail;
    uint64_t owner_thread;

    ztk_mutex *timer_mtx;
    struct ztk_poller_timer **timer_heap;
    size_t timer_heap_count;
    size_t timer_heap_cap;

    ztk_poller_load_state load;
};

void ztk_poller_task_init(struct ztk_poller *p);
void ztk_poller_task_fini(struct ztk_poller *p);

void ztk_poller_timer_init(struct ztk_poller *p);
void ztk_poller_timer_fini(struct ztk_poller *p);

void ztk_poller_drain_tasks(struct ztk_poller *p);

/** 计算 epoll_wait 超时（毫秒），-1 表示无限 */
int ztk_poller_resolve_timeout_ms(struct ztk_poller *p, int request_ms);

ztk_err_t ztk_poller_async_impl(struct ztk_poller *p, ztk_poller_task_fn fn, void *user, int may_sync,
                                  int push_front);

void ztk_poller_timer_insert_locked(ztk_poller *p, ztk_poller_timer *timer, uint64_t deadline_ms);

void ztk_poller_load_init(ztk_poller *p);
void ztk_poller_load_fini(ztk_poller *p);
void ztk_poller_load_on_sleep(ztk_poller *p);
void ztk_poller_load_on_wake(ztk_poller *p);
int ztk_poller_load_percent(ztk_poller *p);

#endif /* ZTK_POLLER_INTERNAL_H */
