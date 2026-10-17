/**
 * [03] 单播 — 接收端
 * API: ztk_udp_server（无 udp_session，回调为 on_packet）
 *
 * 运行: ./demo_03_udp_unicast_recv
 * 测试: ./demo_04_udp_unicast_send 127.0.0.1 9901
 */
#include "ztk/net/udp_server.h"
#include "ztk/poller/poller.h"
#include "ztk/thread/thread.h"
#include "demo_udp_common.h"
#include <stdio.h>
#include <stdlib.h>

static volatile int g_stop;

static void on_packet(ztk_udp_server *srv, const char *ip, uint16_t port, const void *data, size_t len,
                      void *user)
{
    (void)srv;
    (void)user;
    demo_udp_print_packet("UDP unicast", ip, port, data, len);
}

static void poller_entry(void *arg)
{
    ztk_poller_run((ztk_poller *)arg, &g_stop);
}

int main(void)
{
    ztk_platform_init();
    demo_udp_banner("06", "unicast recv (ztk_udp_server)");

    ztk_poller *p = ztk_poller_create();
    if (!p)
        return 1;

    ztk_udp_server_ops_t ops = { on_packet };
    ztk_udp_server_opts_t opts = {
        .host = "0.0.0.0",
        .port = DEMO_UDP_UNICAST_PORT,
        .reuse = 1,
        .poller = p,
        .ops = &ops,
    };

    ztk_udp_server *srv = ztk_udp_server_create(&opts);
    if (!srv || ztk_udp_server_start(srv) != ZTK_OK) {
        fprintf(stderr, "[06] udp_server start failed\n");
        ztk_poller_destroy(p);
        return 1;
    }

    printf("[UDP unicast] listening on 0.0.0.0:%u\n", (unsigned)ztk_udp_server_port(srv));

    ztk_thread *t = ztk_thread_create(poller_entry, p);
    if (!t) {
        ztk_udp_server_destroy(srv);
        ztk_poller_destroy(p);
        return 1;
    }

    printf("[06] exits in ~300 seconds\n");
    demo_run_seconds(&g_stop, 300);

    g_stop = 1;
    ztk_thread_join(t);
    ztk_thread_destroy(t);
    ztk_udp_server_destroy(srv);
    ztk_poller_destroy(p);
    return 0;
}
