#include "ztk/util/mpsc.h"
#include "ztk/ztk_errno.h"
#include <stdint.h>
#include <stdlib.h>

#if defined(_MSC_VER)
#  include <windows.h>
static uint64_t ztk_atomic_load64(volatile LONG64 *v)
{
    return (uint64_t)InterlockedCompareExchange64(v, 0, 0);
}
static void ztk_atomic_store64(volatile LONG64 *v, uint64_t val)
{
    InterlockedExchange64(v, (LONG64)val);
}
static int ztk_atomic_cas64(volatile LONG64 *v, uint64_t *expected, uint64_t desired)
{
    LONG64 exp = (LONG64)*expected;
    LONG64 got = InterlockedCompareExchange64(v, (LONG64)desired, exp);
    if ((uint64_t)got == *expected)
        return 1;
    *expected = (uint64_t)got;
    return 0;
}
#elif defined(__GNUC__) || defined(__clang__)
static uint64_t ztk_atomic_load64(volatile uint64_t *v)
{
    return __atomic_load_n(v, __ATOMIC_SEQ_CST);
}
static void ztk_atomic_store64(volatile uint64_t *v, uint64_t val)
{
    __atomic_store_n(v, val, __ATOMIC_SEQ_CST);
}
static int ztk_atomic_cas64(volatile uint64_t *v, uint64_t *expected, uint64_t desired)
{
    return __atomic_compare_exchange_n(v, expected, desired, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
#else
#  error "ztk_mpsc requires MSVC or GCC/clang atomics"
#endif

typedef struct ztk_mpsc_cell {
    void *data;
#if defined(_MSC_VER)
    volatile LONG64 seq;
#else
    volatile uint64_t seq;
#endif
} ztk_mpsc_cell;

struct ztk_mpsc_queue {
    ztk_mpsc_cell *buf;
    size_t cap;
    size_t mask;
#if defined(_MSC_VER)
    volatile LONG64 head;
    volatile LONG64 tail;
#else
    volatile uint64_t head;
    volatile uint64_t tail;
#endif
};

static int is_pow2(size_t n)
{
    return n >= 2 && (n & (n - 1)) == 0;
}

ztk_mpsc_queue *ztk_mpsc_create(size_t cap_pow2)
{
    ztk_mpsc_queue *q;
    size_t i;

    if (!is_pow2(cap_pow2))
        return NULL;

    q = (ztk_mpsc_queue *)calloc(1, sizeof(*q));
    if (!q)
        return NULL;

    q->buf = (ztk_mpsc_cell *)calloc(cap_pow2, sizeof(*q->buf));
    if (!q->buf) {
        free(q);
        return NULL;
    }

    q->cap = cap_pow2;
    q->mask = cap_pow2 - 1;
    for (i = 0; i < cap_pow2; ++i)
        ztk_atomic_store64(&q->buf[i].seq, (uint64_t)i);
    ztk_atomic_store64(&q->head, 0);
    ztk_atomic_store64(&q->tail, 0);
    return q;
}

void ztk_mpsc_destroy(ztk_mpsc_queue *q)
{
    if (!q)
        return;
    free(q->buf);
    free(q);
}

ztk_err_t ztk_mpsc_push(ztk_mpsc_queue *q, void *item)
{
    ztk_mpsc_cell *cell;
    uint64_t pos;

    if (!q)
        return ZTK_ERR_INVALID;

    pos = ztk_atomic_load64(&q->tail);
    for (;;) {
        uint64_t seq;
        int64_t dif;

        cell = &q->buf[(size_t)(pos & q->mask)];
        seq = ztk_atomic_load64(&cell->seq);
        dif = (int64_t)seq - (int64_t)pos;
        if (dif == 0) {
            if (ztk_atomic_cas64(&q->tail, &pos, pos + 1))
                break;
        } else if (dif < 0) {
            return ZTK_ERR_AGAIN;
        } else {
            pos = ztk_atomic_load64(&q->tail);
        }
    }

    cell->data = item;
    ztk_atomic_store64(&cell->seq, pos + 1);
    return ZTK_OK;
}

void *ztk_mpsc_pop(ztk_mpsc_queue *q)
{
    ztk_mpsc_cell *cell;
    uint64_t pos;
    uint64_t seq;
    int64_t dif;
    void *data;

    if (!q)
        return NULL;

    pos = ztk_atomic_load64(&q->head);
    cell = &q->buf[(size_t)(pos & q->mask)];
    seq = ztk_atomic_load64(&cell->seq);
    dif = (int64_t)seq - (int64_t)(pos + 1);
    if (dif != 0)
        return NULL;

    data = cell->data;
    ztk_atomic_store64(&cell->seq, pos + q->mask + 1);
    ztk_atomic_store64(&q->head, pos + 1);
    return data;
}
