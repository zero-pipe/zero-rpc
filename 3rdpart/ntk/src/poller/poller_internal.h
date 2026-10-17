#ifndef NTK_POLLER_INTERNAL_H
#define NTK_POLLER_INTERNAL_H

#include "thread/sync.h"
#include <stddef.h>
#include <stdint.h>

typedef struct ntk_poller ntk_poller;

typedef void (*ntk_poller_task_fn)(void *user);
typedef uint64_t (*ntk_poller_delay_cb)(void *user);

typedef struct ntk_poller_task {
    ntk_poller_task_fn fn;
    void *user;
    struct ntk_poller_task *next;
} ntk_poller_task;

struct ntk_poller_timer {
    ntk_poller *poller;
    ntk_poller_delay_cb cb;
    void *user;
    int cancelled;
    unsigned refs;
    uint64_t deadline_ms;
    size_t heap_index;
};

typedef struct ntk_poller_timer ntk_poller_timer;

typedef struct ntk_fd_entry ntk_fd_entry;

typedef struct ntk_poller_load_state {
    ntk_mutex *mtx;
    uint64_t last_ms;
    uint64_t run_ms;
    uint64_t sleep_ms;
    int sleeping;
} ntk_poller_load_state;

struct ntk_buf_pool;

struct ntk_poller {
    intptr_t epfd;
    int wake_r;
    int wake_w;
    ntk_fd_entry *entries;

    /** 可选：per-poller 字节池（attach 挂载；poller 不拥有生命周期；建议 thread_safe=0） */
    struct ntk_buf_pool *buf_pool;

    ntk_mutex *task_mtx;
    ntk_poller_task *task_head;
    ntk_poller_task *task_tail;
    uint64_t owner_thread;

    ntk_mutex *timer_mtx;
    struct ntk_poller_timer **timer_heap;
    size_t timer_heap_count;
    size_t timer_heap_cap;

    ntk_poller_load_state load;
};

void ntk_poller_task_init(struct ntk_poller *p);
void ntk_poller_task_fini(struct ntk_poller *p);

void ntk_poller_timer_init(struct ntk_poller *p);
void ntk_poller_timer_fini(struct ntk_poller *p);

void ntk_poller_drain_tasks(struct ntk_poller *p);

/** 计算 epoll_wait 超时（毫秒），-1 表示无限 */
int ntk_poller_resolve_timeout_ms(struct ntk_poller *p, int request_ms);

ntk_err_t ntk_poller_async_impl(struct ntk_poller *p, ntk_poller_task_fn fn, void *user, int may_sync,
                                  int push_front);

void ntk_poller_timer_insert_locked(ntk_poller *p, ntk_poller_timer *timer, uint64_t deadline_ms);

void ntk_poller_load_init(ntk_poller *p);
void ntk_poller_load_fini(ntk_poller *p);
void ntk_poller_load_on_sleep(ntk_poller *p);
void ntk_poller_load_on_wake(ntk_poller *p);
int ntk_poller_load_percent(ntk_poller *p);

#endif /* NTK_POLLER_INTERNAL_H */
