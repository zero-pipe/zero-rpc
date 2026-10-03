#include "zrpc_internal.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
static ztk_sem *g_sem;
static int g_task_ok;
static void *g_remote_obj;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail = 1;                                            \
        }                                                          \
    } while (0)

/* 在 poller 线程内验证：当前 poller 正确，且池释放后可复用同一块。 */
static void pool_probe(void *user) {
    zrpc_node_t *node = (zrpc_node_t *)user;
    ztk_poller *cur = ztk_poller_current();
    ztk_buf_pool *pool;
    void *a;
    void *b;
    size_t cap_a = 0;
    size_t cap_b = 0;

    g_task_ok = 0;
    if (cur != node->poller) {
        goto done;
    }
    pool = ztk_poller_buf_pool(cur);
    if (!pool) {
        goto done;
    }
    a = ztk_buf_pool_acquire(pool, 128, &cap_a);
    if (!a) {
        goto done;
    }
    ztk_buf_pool_release(pool, a, cap_a);
    b = ztk_buf_pool_acquire(pool, 128, &cap_b);
    if (b != a) {
        ztk_buf_pool_release(pool, b, cap_b);
        goto done;
    }
    ztk_buf_pool_release(pool, b, cap_b);

    /* zrpc_mem：owner 线程内释放应复用同一块。 */
    {
        void *m1 = zrpc_mem_alloc(64);
        void *m2;
        if (!m1) {
            goto done;
        }
        zrpc_mem_free(m1);
        m2 = zrpc_mem_alloc(64);
        if (m2 != m1) {
            zrpc_mem_free(m2);
            goto done;
        }
        zrpc_mem_free(m2);
    }
    /* 交给主线程跨线程释放：不得崩溃。 */
    g_remote_obj = zrpc_mem_alloc(64);
    if (!g_remote_obj) {
        goto done;
    }
    g_task_ok = 1;
done:
    ztk_sem_post(g_sem, 1);
}

int main(void) {
    zrpc_node_config_t cfg;
    zrpc_node_t *node = NULL;
    unsigned i;
    unsigned n;

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "pooltest";
    cfg.transport = ZRPC_TRANSPORT_UDP;
    cfg.bind_host = "127.0.0.1";
    cfg.mode = ZRPC_MODE_STATIC;
    cfg.io_threads = 2;

    /* 主线程未绑定 poller。 */
    CHECK(ztk_poller_current() == NULL);

    if (zrpc_node_create(&cfg, &node) != ZRPC_OK) {
        printf("node create failed\n");
        return 1;
    }

    n = ztk_poller_pool_size(node->pool);
    CHECK(n >= 1);
    for (i = 0; i < n; i++) {
        ztk_poller *p = ztk_poller_pool_at(node->pool, i);
        CHECK(p != NULL);
        CHECK(ztk_poller_buf_pool(p) != NULL);
    }

    /* 在 owner poller 线程内验证 current + 复用。 */
    g_sem = ztk_sem_create(0);
    CHECK(g_sem != NULL);
    if (g_sem) {
        CHECK(ztk_poller_async(node->poller, pool_probe, node, 0) == ZTK_OK);
        CHECK(ztk_sem_timedwait(g_sem, 2000) == ZTK_OK);
        CHECK(g_task_ok == 1);
        ztk_sem_destroy(g_sem);
        g_sem = NULL;
    }

    /* 跨线程释放（非 owner poller 线程）。 */
    if (g_remote_obj) {
        zrpc_mem_free(g_remote_obj);
        g_remote_obj = NULL;
    }

    zrpc_node_destroy(node);
    printf(g_fail ? "POOL FAIL\n" : "POOL OK\n");
    return g_fail ? 1 : 0;
}
