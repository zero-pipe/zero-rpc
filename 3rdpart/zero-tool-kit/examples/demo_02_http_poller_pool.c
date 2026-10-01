/**
 * [02] HTTP — poller_pool（2 个 I/O 线程）+ TcpServer
 * 端口：DEMO_HTTP_PORT（默认 80）
 */
#include "ztk/net/tcp_server.h"
#include "ztk/poller/poller_pool.h"
#include "demo_common.h"
#include "demo_udp_common.h"
#include <stdio.h>
#include <stdlib.h>

static volatile int g_stop;

static void http_on_recv(ztk_tcp_session *session, const void *data, size_t len, void *user)
{
    (void)user;
    demo_on_recv_print("http: message received", data, len);
    ztk_tcp_session_send(session, data, len);
}

static void http_on_error(ztk_tcp_session *session, void *user)
{
    (void)session;
    (void)user;
    demo_on_disconnect_print("HTTP");
}

int main(void)
{
    ztk_platform_init();
    demo_print_banner("02", "HTTP TcpServer + poller_pool size=2");

    ztk_poller_pool_opts_t popts = { .size = 2 };
    ztk_poller_pool *pool = ztk_poller_pool_create(&popts);
    if (!pool || ztk_poller_pool_start(pool) != ZTK_OK) {
        fprintf(stderr, "[02] poller_pool start failed\n");
        if (pool)
            ztk_poller_pool_destroy(pool);
        return 1;
    }

    ztk_tcp_session_ops_t ops = { http_on_recv, http_on_error, NULL };
    ztk_tcp_server_opts_t opts = {
        .host = "0.0.0.0",
        .port = DEMO_HTTP_PORT,
        .backlog = 128,
        .poller_pool = pool,
        .session_ops = &ops,
        .manager_interval_sec = -1.0f,
    };

    ztk_tcp_server *srv = ztk_tcp_server_create(&opts);
    if (!srv || ztk_tcp_server_start(srv) != ZTK_OK) {
        fprintf(stderr, "[02] HTTP start failed(port 80 may need root; try -DDEMO_HTTP_PORT=8080)\n");
        ztk_poller_pool_stop(pool);
        ztk_poller_pool_destroy(pool);
        return 1;
    }

    demo_print_listen("HTTP", ztk_tcp_server_port(srv));
    printf("[02] %u poller threads from pool; pick_poller load-balances new sessions\n",
           ztk_poller_pool_size(pool));
    printf("[02] auto exit in ~300 seconds\n");

    demo_run_seconds(&g_stop, 300);

    ztk_tcp_server_destroy(srv);
    ztk_poller_pool_stop(pool);
    ztk_poller_pool_destroy(pool);
    return 0;
}
