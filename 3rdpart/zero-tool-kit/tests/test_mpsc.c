#include "ztk/util/mpsc.h"
#include "ztk/thread/thread.h"
#include "ztk/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* single-thread: 顺序 push/pop，验证 FIFO 顺序和容量满时 AGAIN 语义  */
/* ------------------------------------------------------------------ */
static int test_single_thread(void)
{
    ztk_mpsc_queue *q = ztk_mpsc_create(64);
    int i;

    if (!q)
        return 1;

    /* push 64 条（满），第 65 条应返回 AGAIN */
    for (i = 0; i < 64; ++i) {
        /* +1 确保 payload 非 NULL（pop 返回 NULL 表示队列空） */
        if (ztk_mpsc_push(q, (void *)(uintptr_t)(unsigned)(i + 1)) != ZTK_OK) {
            ztk_mpsc_destroy(q);
            return 1;
        }
    }
    if (ztk_mpsc_push(q, (void *)(uintptr_t)1) != ZTK_ERR_AGAIN) {
        fprintf(stderr, "single: full queue should return AGAIN\n");
        ztk_mpsc_destroy(q);
        return 1;
    }

    /* pop 64 条，验证 FIFO */
    for (i = 0; i < 64; ++i) {
        void *v = ztk_mpsc_pop(q);
        if ((uintptr_t)v != (uintptr_t)(unsigned)(i + 1)) {
            fprintf(stderr, "single: FIFO mismatch at %d: got %zu want %u\n",
                    i, (size_t)(uintptr_t)v, (unsigned)(i + 1));
            ztk_mpsc_destroy(q);
            return 1;
        }
    }
    if (ztk_mpsc_pop(q) != NULL) {
        fprintf(stderr, "single: empty queue should return NULL\n");
        ztk_mpsc_destroy(q);
        return 1;
    }

    ztk_mpsc_destroy(q);
    return 0;
}

/* ------------------------------------------------------------------ */
/* multi-producer: N 生产者各推 M 条，1 消费者统计总数               */
/* ------------------------------------------------------------------ */
#define MPSC_PRODUCERS 4
#define MPSC_PER_PROD  10000
#define MPSC_CAP       4096

typedef struct prod_ctx {
    ztk_mpsc_queue *q;
    int id;
} prod_ctx;

static volatile long g_popped;

static void producer_fn(void *user)
{
    prod_ctx *ctx = (prod_ctx *)user;
    int i;
    for (i = 0; i < MPSC_PER_PROD; ++i) {
        /* +1 确保 payload 非 NULL */
        uintptr_t v = (uintptr_t)(((unsigned)ctx->id << 16) | (unsigned)i) + 1;
        while (ztk_mpsc_push(ctx->q, (void *)v) == ZTK_ERR_AGAIN)
            ztk_sleep_ms(0);
    }
}

static void consumer_fn(void *user)
{
    ztk_mpsc_queue *q = (ztk_mpsc_queue *)user;
    long target = (long)MPSC_PRODUCERS * MPSC_PER_PROD;
    long popped = 0;
    while (popped < target) {
        void *item = ztk_mpsc_pop(q);
        if (item)
            ++popped;
        else
            ztk_sleep_ms(0);
    }
    g_popped = popped;
}

static int test_multi_producer(void)
{
    ztk_mpsc_queue *q;
    prod_ctx ctx[MPSC_PRODUCERS];
    ztk_thread *prods[MPSC_PRODUCERS];
    ztk_thread *cons;
    int i;

    q = ztk_mpsc_create(MPSC_CAP);
    if (!q)
        return 1;

    for (i = 0; i < MPSC_PRODUCERS; ++i)
        prods[i] = NULL;
    cons = NULL;
    g_popped = 0;

    cons = ztk_thread_create(consumer_fn, q);
    if (!cons)
        goto fail;

    for (i = 0; i < MPSC_PRODUCERS; ++i) {
        ctx[i].q  = q;
        ctx[i].id = i;
        prods[i]  = ztk_thread_create(producer_fn, &ctx[i]);
        if (!prods[i])
            goto fail_join;
    }

    for (i = 0; i < MPSC_PRODUCERS; ++i) {
        ztk_thread_join(prods[i]);
        ztk_thread_destroy(prods[i]);
        prods[i] = NULL;
    }
    ztk_thread_join(cons);
    ztk_thread_destroy(cons);
    cons = NULL;

    if (g_popped != (long)MPSC_PRODUCERS * MPSC_PER_PROD) {
        fprintf(stderr, "multi: count mismatch popped=%ld want=%d\n",
                g_popped, MPSC_PRODUCERS * MPSC_PER_PROD);
        ztk_mpsc_destroy(q);
        return 1;
    }

    ztk_mpsc_destroy(q);
    return 0;

fail_join:
    for (i = 0; i < MPSC_PRODUCERS; ++i) {
        if (prods[i]) {
            ztk_thread_join(prods[i]);
            ztk_thread_destroy(prods[i]);
        }
    }
    if (cons) {
        ztk_thread_join(cons);
        ztk_thread_destroy(cons);
    }
fail:
    ztk_mpsc_destroy(q);
    return 1;
}

int main(void)
{
    if (test_single_thread() != 0) {
        fprintf(stderr, "single-thread mpsc failed\n");
        return 1;
    }
    if (test_multi_producer() != 0) {
        fprintf(stderr, "multi-producer mpsc failed\n");
        return 1;
    }
    printf("test_mpsc ok\n");
    return 0;
}
