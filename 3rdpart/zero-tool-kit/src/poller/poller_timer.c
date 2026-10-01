#include "poller_internal.h"
#include "ztk/poller/poller.h"
#include "ztk/platform.h"
#include "ztk/ztk_errno.h"
#include <stdlib.h>
#include <string.h>

typedef struct delay_insert_ctx {
    ztk_poller *poller;
    ztk_poller_timer *timer;
    uint64_t deadline_ms;
} delay_insert_ctx;

/** 单次 poll 最多处理的 timer 个数，防止饿死 accept/async */
#define ZTK_TIMER_FLUSH_MAX 256

#define ZTK_TIMER_HEAP_INDEX_NONE ((size_t)-1)

static void timer_unref_locked(ztk_poller_timer *timer)
{
    if (timer && timer->refs > 0 && --timer->refs == 0)
        free(timer);
}

static int timer_heap_reserve(ztk_poller *p, size_t need)
{
    ztk_poller_timer **nb;
    size_t cap = p->timer_heap_cap ? p->timer_heap_cap : 8;

    if (need <= p->timer_heap_cap)
        return 0;
    while (cap < need)
        cap *= 2;
    nb = (ztk_poller_timer **)realloc(p->timer_heap, cap * sizeof(*nb));
    if (!nb)
        return -1;
    p->timer_heap = nb;
    p->timer_heap_cap = cap;
    return 0;
}

static void timer_heap_swap(ztk_poller *p, size_t i, size_t j)
{
    ztk_poller_timer *tmp = p->timer_heap[i];
    p->timer_heap[i] = p->timer_heap[j];
    p->timer_heap[j] = tmp;
    p->timer_heap[i]->heap_index = i;
    p->timer_heap[j]->heap_index = j;
}

static void timer_heap_bubble_up(ztk_poller *p, size_t i)
{
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (p->timer_heap[parent]->deadline_ms <= p->timer_heap[i]->deadline_ms)
            break;
        timer_heap_swap(p, parent, i);
        i = parent;
    }
}

static void timer_heap_bubble_down(ztk_poller *p, size_t i)
{
    for (;;) {
        size_t left = i * 2 + 1;
        size_t right = left + 1;
        size_t smallest = i;

        if (left < p->timer_heap_count &&
            p->timer_heap[left]->deadline_ms < p->timer_heap[smallest]->deadline_ms)
            smallest = left;
        if (right < p->timer_heap_count &&
            p->timer_heap[right]->deadline_ms < p->timer_heap[smallest]->deadline_ms)
            smallest = right;
        if (smallest == i)
            break;
        timer_heap_swap(p, i, smallest);
        i = smallest;
    }
}

static int timer_heap_push(ztk_poller *p, ztk_poller_timer *timer)
{
    if (timer_heap_reserve(p, p->timer_heap_count + 1) != 0)
        return -1;
    timer->heap_index = p->timer_heap_count;
    p->timer_heap[p->timer_heap_count++] = timer;
    timer_heap_bubble_up(p, timer->heap_index);
    return 0;
}

static ztk_poller_timer *timer_heap_pop_min(ztk_poller *p)
{
    ztk_poller_timer *top;

    if (!p || p->timer_heap_count == 0)
        return NULL;
    top = p->timer_heap[0];
    top->heap_index = ZTK_TIMER_HEAP_INDEX_NONE;
    --p->timer_heap_count;
    if (p->timer_heap_count > 0) {
        p->timer_heap[0] = p->timer_heap[p->timer_heap_count];
        p->timer_heap[0]->heap_index = 0;
        timer_heap_bubble_down(p, 0);
    }
    return top;
}

static void timer_heap_remove(ztk_poller *p, ztk_poller_timer *timer)
{
    size_t i = timer->heap_index;

    if (!p || !timer || i == ZTK_TIMER_HEAP_INDEX_NONE || i >= p->timer_heap_count)
        return;

    timer->heap_index = ZTK_TIMER_HEAP_INDEX_NONE;
    --p->timer_heap_count;
    if (i < p->timer_heap_count) {
        p->timer_heap[i] = p->timer_heap[p->timer_heap_count];
        p->timer_heap[i]->heap_index = i;
        timer_heap_bubble_down(p, i);
        timer_heap_bubble_up(p, i);
    }
}

void ztk_poller_timer_init(ztk_poller *p)
{
    if (!p)
        return;
    p->timer_mtx = ztk_mutex_create(ZTK_MUTEX_NORMAL);
    p->timer_heap = NULL;
    p->timer_heap_count = 0;
    p->timer_heap_cap = 0;
}

void ztk_poller_timer_fini(ztk_poller *p)
{
    size_t i;

    if (!p)
        return;
    ztk_mutex_lock(p->timer_mtx);
    for (i = 0; i < p->timer_heap_count; ++i)
        timer_unref_locked(p->timer_heap[i]);
    free(p->timer_heap);
    p->timer_heap = NULL;
    p->timer_heap_count = 0;
    p->timer_heap_cap = 0;
    ztk_mutex_unlock(p->timer_mtx);
    if (p->timer_mtx) {
        ztk_mutex_destroy(p->timer_mtx);
        p->timer_mtx = NULL;
    }
}

void ztk_poller_timer_insert_locked(ztk_poller *p, ztk_poller_timer *timer, uint64_t deadline_ms)
{
    if (!p || !timer)
        return;
    timer->deadline_ms = deadline_ms;
    if (timer->heap_index != ZTK_TIMER_HEAP_INDEX_NONE)
        timer_heap_remove(p, timer);
    (void)timer_heap_push(p, timer);
}

static void delay_insert_async(void *user)
{
    delay_insert_ctx *ctx = (delay_insert_ctx *)user;
    ztk_mutex_lock(ctx->poller->timer_mtx);
    ztk_poller_timer_insert_locked(ctx->poller, ctx->timer, ctx->deadline_ms);
    ztk_mutex_unlock(ctx->poller->timer_mtx);
    free(ctx);
}

static void unlink_timer(ztk_poller *p, ztk_poller_timer *timer)
{
    if (!timer)
        return;
    timer_heap_remove(p, timer);
}

static int64_t flush_expired_timers(ztk_poller *p, uint64_t now_ms)
{
    int processed = 0;

    while (processed < ZTK_TIMER_FLUSH_MAX && p->timer_heap_count > 0 &&
           p->timer_heap[0]->deadline_ms <= now_ms) {
        ztk_poller_timer *timer = timer_heap_pop_min(p);

        ++processed;
        if (!timer || timer->cancelled) {
            timer_unref_locked(timer);
            continue;
        }

        {
            uint64_t next_ms = 0;
            if (timer->cb)
                next_ms = timer->cb(timer->user);

            if (next_ms > 0 && !timer->cancelled) {
                ztk_poller_timer_insert_locked(p, timer, now_ms + next_ms);
            } else {
                timer_unref_locked(timer);
            }
        }
    }

    if (p->timer_heap_count == 0)
        return -1;
    if (p->timer_heap[0]->deadline_ms > now_ms)
        return (int64_t)(p->timer_heap[0]->deadline_ms - now_ms);
    return 0;
}

int ztk_poller_resolve_timeout_ms(ztk_poller *p, int request_ms)
{
    if (!p || !p->timer_mtx)
        return request_ms;

    ztk_mutex_lock(p->timer_mtx);
    uint64_t now = ztk_monotonic_ms();
    int64_t delay = flush_expired_timers(p, now);
    ztk_mutex_unlock(p->timer_mtx);

    if (delay < 0)
        return request_ms;
    if (request_ms < 0)
        return (int)(delay > 0x7fffffff ? 0x7fffffff : delay);
    if (request_ms == 0)
        return 0;
    return (int)(delay < request_ms ? delay : request_ms);
}

ztk_poller_timer *ztk_poller_do_delay(ztk_poller *p, uint64_t delay_ms, ztk_poller_delay_cb cb, void *user)
{
    if (!p || !cb || !p->timer_mtx)
        return NULL;

    ztk_poller_timer *timer = (ztk_poller_timer *)calloc(1, sizeof(*timer));
    if (!timer)
        return NULL;
    timer->poller = p;
    timer->cb = cb;
    timer->user = user;
    timer->heap_index = ZTK_TIMER_HEAP_INDEX_NONE;
    timer->refs = 1;

    uint64_t deadline = ztk_monotonic_ms() + delay_ms;

    if (p->owner_thread != 0 && p->owner_thread == ztk_thread_self_id()) {
        ztk_mutex_lock(p->timer_mtx);
        ztk_poller_timer_insert_locked(p, timer, deadline);
        ztk_mutex_unlock(p->timer_mtx);
        ztk_poller_wake(p);
        return timer;
    }

    delay_insert_ctx *ctx = (delay_insert_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        free(timer);
        return NULL;
    }
    ctx->poller = p;
    ctx->timer = timer;
    ctx->deadline_ms = deadline;
    if (ztk_poller_async_first(p, delay_insert_async, ctx, 0) != ZTK_OK) {
        free(ctx);
        free(timer);
        return NULL;
    }
    return timer;
}

typedef struct timer_cancel_ctx {
    ztk_poller_timer *timer;
} timer_cancel_ctx;

static void timer_cancel_async(void *user)
{
    timer_cancel_ctx *ctx = (timer_cancel_ctx *)user;
    ztk_poller_timer *timer = ctx->timer;
    ztk_poller *p = timer->poller;
    free(ctx);
    ztk_mutex_lock(p->timer_mtx);
    if (timer->heap_index != ZTK_TIMER_HEAP_INDEX_NONE) {
        unlink_timer(p, timer);
        timer_unref_locked(timer); /* release heap ownership */
    }
    timer_unref_locked(timer); /* release cancellation task ownership */
    ztk_mutex_unlock(p->timer_mtx);
}

void ztk_poller_timer_cancel(ztk_poller_timer *timer)
{
    if (!timer)
        return;
    ztk_poller *p = timer->poller;
    if (!p || !p->timer_mtx)
        return;

    if (p->owner_thread != 0 && p->owner_thread == ztk_thread_self_id()) {
        ztk_mutex_lock(p->timer_mtx);
        timer->cancelled = 1;
        unlink_timer(p, timer);
        timer_unref_locked(timer);
        ztk_mutex_unlock(p->timer_mtx);
        return;
    }

    ztk_mutex_lock(p->timer_mtx);
    timer->cancelled = 1;
    timer->refs++; /* held by the cancellation task */
    ztk_mutex_unlock(p->timer_mtx);

    timer_cancel_ctx *ctx = (timer_cancel_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        ztk_mutex_lock(p->timer_mtx);
        timer_unref_locked(timer);
        ztk_mutex_unlock(p->timer_mtx);
        return;
    }
    ctx->timer = timer;
    if (ztk_poller_async(p, timer_cancel_async, ctx, 0) != ZTK_OK) {
        free(ctx);
        ztk_mutex_lock(p->timer_mtx);
        timer_unref_locked(timer);
        ztk_mutex_unlock(p->timer_mtx);
    }
}
