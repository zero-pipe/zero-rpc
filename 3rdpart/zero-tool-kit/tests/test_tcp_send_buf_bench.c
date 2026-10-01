/**
 * 微基准：1 切片 × N 连接共享 ztk_buf，出站 mem 拷贝应远小于 N×切片（理想为 0）。
 */
#include "ztk/net/tcp_server.h"
#include "ztk/net/socket.h"
#include "ztk/util/buf.h"
#include "ztk/poller/poller_pool.h"
#include <stdio.h>
#include <string.h>
#include "test_portable.h"

#define BENCH_CLIENTS  100
#define BENCH_SEG_SIZE 65536u

static ztk_buf *g_seg;
static volatile int g_served;

static void bench_on_recv(ztk_tcp_session *session, const void *data, size_t len, void *user)
{
    (void)data;
    (void)len;
    (void)user;
    if (ztk_tcp_session_send_buf(session, g_seg) == ZTK_OK)
        ++g_served;
}

static ztk_err_t wait_connect(ztk_socket *sock, int timeout_ms)
{
    for (int t = 0; t < timeout_ms; t += 10) {
        ztk_err_t err = ztk_socket_check_connect(sock);
        if (err == ZTK_OK)
            return ZTK_OK;
        if (err != ZTK_ERR_AGAIN)
            return err;
        ztk_test_sleep_ms(10);
    }
    return ztk_socket_check_connect(sock);
}

int main(void)
{
    ztk_poller_pool_opts_t popts = { .size = 2, .prefer_current_thread = 0,
        .thread_priority = ZTK_THREAD_PRIO_NORMAL };
    ztk_poller_pool *pool = ztk_poller_pool_create(&popts);
    ztk_tcp_session_ops_t ops = { bench_on_recv, NULL, NULL };
    ztk_tcp_server_opts_t opts = {
        .host = "127.0.0.1",
        .port = 0,
        .backlog = 128,
        .poller_pool = pool,
        .session_ops = &ops,
        .manager_interval_sec = -1.0f
    };
    ztk_tcp_server *srv;
    char expect[BENCH_SEG_SIZE];
    unsigned i;
    size_t appended;

    if (!pool || ztk_poller_pool_start(pool) != ZTK_OK) {
        if (pool)
            ztk_poller_pool_destroy(pool);
        return 1;
    }

    g_seg = ztk_buf_alloc(BENCH_SEG_SIZE);
    if (!g_seg)
        return 1;
    memset(expect, 0x5A, sizeof(expect));
    memcpy((void *)ztk_buf_data(g_seg), expect, sizeof(expect));
    ztk_buf_set_len(g_seg, sizeof(expect));

    srv = ztk_tcp_server_create(&opts);
    if (!srv || ztk_tcp_server_start(srv) != ZTK_OK)
        return 1;

    ztk_tcp_test_reset_out_mem_append();
    g_served = 0;

    for (i = 0; i < BENCH_CLIENTS; ++i) {
        ztk_socket *cli = ztk_socket_create();
        char buf[4096];
        size_t got = 0;
        int pending = 0;
        ztk_ssize_t rn;

        if (!cli)
            return 1;
        if (ztk_socket_connect(cli, "127.0.0.1", ztk_tcp_server_port(srv), &pending) != ZTK_OK) {
            ztk_socket_destroy(cli);
            return 1;
        }
        if (pending && wait_connect(cli, 5000) != ZTK_OK) {
            ztk_socket_destroy(cli);
            return 1;
        }

        ztk_socket_send(cli, "x", 1);

        for (int w = 0; w < 500 && got < BENCH_SEG_SIZE; ++w) {
            rn = ztk_socket_recv(cli, buf, sizeof(buf));
            if (rn > 0) {
                if (memcmp(buf, expect + got, (size_t)rn) != 0)
                    return 1;
                got += (size_t)rn;
            } else
                ztk_test_sleep_ms(5);
        }

        ztk_socket_destroy(cli);
        if (got != BENCH_SEG_SIZE) {
            fprintf(stderr, "client %u recv %zu\n", i, got);
            return 1;
        }
    }

    appended = ztk_tcp_test_out_mem_append_bytes();
    if (g_served < BENCH_CLIENTS) {
        fprintf(stderr, "served=%d\n", g_served);
        return 1;
    }
    if (ztk_buf_refcnt(g_seg) != 1) {
        fprintf(stderr, "seg refcnt=%d\n", ztk_buf_refcnt(g_seg));
        return 1;
    }
    if (appended >= BENCH_SEG_SIZE) {
        fprintf(stderr, "out mem append=%zu (expect < %u)\n", appended, BENCH_SEG_SIZE);
        return 1;
    }

    ztk_tcp_server_destroy(srv);
    ztk_poller_pool_stop(pool);
    ztk_poller_pool_destroy(pool);
    ztk_buf_unref(g_seg);
    printf("test_tcp_send_buf_bench ok (clients=%d append=%zu)\n", BENCH_CLIENTS, appended);
    return 0;
}
