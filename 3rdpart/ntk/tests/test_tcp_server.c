#include "ztk/net/tcp_server.h"
#include "ztk/net/socket.h"
#include "ztk/poller/poller_pool.h"
#include <stdio.h>
#include <string.h>
#include "test_portable.h"

static volatile int g_echo_ok;

static void echo_on_recv(ztk_tcp_session *session, const void *data, size_t len, void *user)
{
    (void)user;
    if (ztk_tcp_session_send(session, data, len) == ZTK_OK)
        g_echo_ok = 1;
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

static int run_echo_poller_pool(unsigned thread_count)
{
    ztk_poller_pool_opts_t popts = { .size = thread_count, .prefer_current_thread = 0,
        .thread_priority = ZTK_THREAD_PRIO_NORMAL };
    ztk_poller_pool *pool = ztk_poller_pool_create(&popts);
    if (!pool || ztk_poller_pool_start(pool) != ZTK_OK)
        return 1;

    ztk_tcp_session_ops_t ops = { echo_on_recv, NULL, NULL };
    ztk_tcp_server_opts_t opts = {
        .host = "127.0.0.1",
        .port = 0,
        .backlog = 64,
        .poller_pool = pool,
        .session_ops = &ops,
        .manager_interval_sec = -1.0f
    };

    ztk_tcp_server *srv = ztk_tcp_server_create(&opts);
    if (!srv || ztk_tcp_server_start(srv) != ZTK_OK) {
        ztk_poller_pool_stop(pool);
        ztk_poller_pool_destroy(pool);
        return 1;
    }

    char lip[64];
    uint16_t port = ztk_tcp_server_port(srv);
    snprintf(lip, sizeof(lip), "127.0.0.1");

    ztk_socket *client = ztk_socket_create();
    int pending = 0;
    if (ztk_socket_connect(client, lip, port, &pending) != ZTK_OK) {
        ztk_tcp_server_destroy(srv);
        ztk_poller_pool_stop(pool);
        ztk_poller_pool_destroy(pool);
        return 1;
    }
    if (pending && wait_connect(client, 3000) != ZTK_OK) {
        ztk_tcp_server_destroy(srv);
        ztk_poller_pool_stop(pool);
        ztk_poller_pool_destroy(pool);
        return 1;
    }

    const char *msg = thread_count > 1 ? "pool" : "hi";
    ztk_socket_send(client, msg, strlen(msg));

    g_echo_ok = 0;
    for (int i = 0; i < 300 && !g_echo_ok; ++i)
        ztk_test_sleep_ms(10);

    ztk_socket_destroy(client);
    ztk_tcp_server_destroy(srv);
    ztk_poller_pool_stop(pool);
    ztk_poller_pool_destroy(pool);

    return g_echo_ok ? 0 : 1;
}

int main(void)
{
    if (run_echo_poller_pool(1) != 0) {
        fprintf(stderr, "poller pool echo (1 thread) failed\n");
        return 1;
    }
    ztk_test_sleep_ms(100);
    if (run_echo_poller_pool(2) != 0) {
        fprintf(stderr, "poller pool echo (2 threads) failed\n");
        return 1;
    }
    printf("test_tcp_server ok\n");
    return 0;
}
