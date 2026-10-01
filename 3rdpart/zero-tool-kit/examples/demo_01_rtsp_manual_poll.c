/**
 * [01] RTSP — 单 poller_pool（size=1），应用显式 start/stop pool
 * 端口：DEMO_RTSP_PORT（默认 554）
 */
#include "ztk/net/tcp_server.h"
#include "ztk/poller/poller_pool.h"
#include "demo_common.h"
#include "demo_udp_common.h"
#include <stdio.h>
#include <stdlib.h>

static volatile int g_stop;

static void rtsp_on_recv(ztk_tcp_session *session, const void *data, size_t len, void *user)
{
    (void)user;
    demo_on_recv_print("rtsp: message received", data, len);
    ztk_tcp_session_send(session, data, len);
}

static void rtsp_on_error(ztk_tcp_session *session, void *user)
{
    (void)session;
    (void)user;
    demo_on_disconnect_print("RTSP");
}

int main(void)
{
    ztk_platform_init();
    demo_print_banner("01", "RTSP with poller_pool size=1");

    ztk_poller_pool_opts_t popts = { .size = 1 };
    ztk_poller_pool *pool = ztk_poller_pool_create(&popts);
    if (!pool || ztk_poller_pool_start(pool) != ZTK_OK) {
        fprintf(stderr, "[01] poller_pool start failed\n");
        if (pool)
            ztk_poller_pool_destroy(pool);
        return 1;
    }

    ztk_tcp_session_ops_t ops = { rtsp_on_recv, rtsp_on_error, NULL };
    ztk_tcp_server_opts_t opts = {
        .host = "0.0.0.0",
        .port = DEMO_RTSP_PORT,
        .backlog = 64,
        .poller_pool = pool,
        .session_ops = &ops,
    };

    ztk_tcp_server *srv = ztk_tcp_server_create(&opts);
    if (!srv || ztk_tcp_server_start(srv) != ZTK_OK) {
        fprintf(stderr, "[01] RTSP start failed(port 554 may need root; try -DDEMO_RTSP_PORT=8554)\n");
        ztk_poller_pool_stop(pool);
        ztk_poller_pool_destroy(pool);
        return 1;
    }

    demo_print_listen("RTSP", ztk_tcp_server_port(srv));
    printf("[01] I/O via ztk_poller_pool (1 thread); auto exit in ~300s\n");

    demo_run_seconds(&g_stop, 300);

    ztk_tcp_server_destroy(srv);
    ztk_poller_pool_stop(pool);
    ztk_poller_pool_destroy(pool);
    return 0;
}
