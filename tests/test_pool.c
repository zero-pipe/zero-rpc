#include "mem.h"
#include "node_access.h"
#include "pending.h"

#include <node.h>

#define NB_LOG_TAG "test"
#include <log.h>

#include <stdio.h>
#include <string.h>

static int g_fail;
static ntk_sem *g_sem;
static int g_task_ok;
static void *g_remote_obj;

static void pending_probe(void *user)
{
    nb_node_t *node = (nb_node_t *)user;
    nb_pending_t *pending[1024];
    unsigned i;

    memset(pending, 0, sizeof(pending));
    for (i = 0; i < sizeof(pending) / sizeof(pending[0]); i++) {
        pending[i] =
            nb_pending_add(node, (uint64_t)i + 1, NULL, NULL, 60000, nb_node_poller(node), NULL);
        if (!pending[i]) {
            g_task_ok = 0;
            ntk_sem_post(g_sem, 1);
            return;
        }
    }
    for (i = 0; i < sizeof(pending) / sizeof(pending[0]); i++) {
        nb_pending_release(pending[i]);
    }
    g_task_ok = 1;
    ntk_sem_post(g_sem, 1);
}

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            nb_log_error("FAIL %s:%d: %s", __FILE__, __LINE__, #cond); \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

/* 在 poller 线程内验证：当前 poller 正确，且池释放后可复用同一块。 */
static void pool_probe(void *user)
{
    nb_node_t *node = (nb_node_t *)user;
    ntk_poller *cur = ntk_poller_current();
    ntk_buf_pool *pool;
    void *a;
    void *b;
    size_t cap_a = 0;
    size_t cap_b = 0;

    g_task_ok = 0;
    if (cur != nb_node_poller(node)) {
        goto done;
    }
    pool = ntk_poller_buf_pool(cur);
    if (!pool) {
        goto done;
    }
    a = ntk_buf_pool_acquire(pool, 128, &cap_a);
    if (!a) {
        goto done;
    }
    ntk_buf_pool_release(pool, a, cap_a);
    b = ntk_buf_pool_acquire(pool, 128, &cap_b);
    if (b != a) {
        ntk_buf_pool_release(pool, b, cap_b);
        goto done;
    }
    ntk_buf_pool_release(pool, b, cap_b);

    /* nb_mem：owner 线程内释放应复用同一块。 */
    {
        void *m1 = nb_mem_alloc(64);
        void *m2;
        if (!m1) {
            goto done;
        }
        nb_mem_free(m1);
        m2 = nb_mem_alloc(64);
        if (m2 != m1) {
            nb_mem_free(m2);
            goto done;
        }
        nb_mem_free(m2);
    }
    /* 交给主线程跨线程释放：不得崩溃。 */
    g_remote_obj = nb_mem_alloc(64);
    if (!g_remote_obj) {
        goto done;
    }
    g_task_ok = 1;
done:
    ntk_sem_post(g_sem, 1);
}

int main(void)
{
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    unsigned i;
    unsigned n;

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "pooltest";
    cfg.transport.transport = NB_TRANSPORT_UDP;
    cfg.transport.bind_host = "127.0.0.1";
    cfg.mode = NB_MODE_STATIC;
    cfg.runtime.io_threads = 2;

    /* 主线程未绑定 poller。 */
    CHECK(ntk_poller_current() == NULL);

    if (nb_node_create(&cfg, &node) != NB_OK) {
        nb_log_error("node create failed");
        return 1;
    }

    n = ntk_poller_pool_size(nb_node_pool(node));
    CHECK(n >= 1);
    for (i = 0; i < n; i++) {
        ntk_poller *p = ntk_poller_pool_at(nb_node_pool(node), i);
        CHECK(p != NULL);
        CHECK(ntk_poller_buf_pool(p) != NULL);
    }

    /* 在 owner poller 线程内验证 current + 复用。 */
    g_sem = ntk_sem_create(0);
    CHECK(g_sem != NULL);
    if (g_sem) {
        CHECK(ntk_poller_async(nb_node_poller(node), pool_probe, node, 0) == NTK_OK);
        CHECK(ntk_sem_timedwait(g_sem, 2000) == NTK_OK);
        CHECK(g_task_ok == 1);
        CHECK(ntk_poller_async(nb_node_poller(node), pending_probe, node, 0) == NTK_OK);
        CHECK(ntk_sem_timedwait(g_sem, 2000) == NTK_OK);
        CHECK(g_task_ok == 1);
        ntk_sem_destroy(g_sem);
        g_sem = NULL;
    }

    /* 跨线程释放（非 owner poller 线程）。 */
    if (g_remote_obj) {
        nb_mem_free(g_remote_obj);
        g_remote_obj = NULL;
    }

    nb_node_destroy(node);
    if (g_fail) {
        nb_log_error("POOL FAIL");
    } else {
        nb_log_info("POOL NB_OK");
    }
    return g_fail ? 1 : 0;
}
