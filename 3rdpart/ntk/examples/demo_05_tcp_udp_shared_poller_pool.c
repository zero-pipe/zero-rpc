/**
 * [05] TCP + UDP share one ztk_poller_pool（显式创建 I/O 线程池）
 *
 * 同一进程、同一组 poller 线程同时 epoll：
 *   - TcpServer  RTSP  (DEMO_RTSP_PORT)
 *   - UdpServer  单播  (DEMO_UDP_UNICAST_PORT)
 *
 * 对比：
 *   demo_01  单 poller，应用自己 ztk_poller_run
 *   demo_02  TcpServer + poller_pool（2 线程）
 *   demo_05  显式 poller_pool，TCP/UDP 共用（ZMS 直播主路径）
 *
 * 测试:
 *   ./demo_05_tcp_udp_shared_poller_pool
 *   echo test | nc 127.0.0.1 8554
 *   echo test | nc -u 127.0.0.1 9901
 */
#include "ztk/net/tcp_server.h"
#include "ztk/net/udp_server.h"
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

static ztk_tcp_server *start_tcp(ztk_poller_pool *pool)
{
    ztk_tcp_session_ops_t ops = { rtsp_on_recv, rtsp_on_error, NULL };
    ztk_tcp_server_opts_t opts = {
        .host = "0.0.0.0",
        .port = DEMO_RTSP_PORT,
        .backlog = 64,
        .poller_pool = pool,
        .session_ops = &ops,
    };
    ztk_tcp_server *srv = ztk_tcp_server_create(&opts);
    if (!srv || ztk_tcp_server_start(srv) != ZTK_OK)
        return NULL;
    demo_print_listen("RTSP/TCP", ztk_tcp_server_port(srv));
    return srv;
}

static void udp_on_packet(ztk_udp_server *srv, const char *ip, uint16_t port, const void *data, size_t len,
                          void *user)
{
    (void)srv;
    (void)user;
    demo_udp_print_packet("UDP unicast (poller pool)", ip, port, data, len);
}

static ztk_udp_server *start_udp(ztk_poller_pool *pool)
{
    ztk_udp_server_ops_t ops = { udp_on_packet };
    ztk_udp_server_opts_t opts = {
        .host = "0.0.0.0",
        .port = DEMO_UDP_UNICAST_PORT,
        .reuse = 1,
        .reuse_port = 1,
        .poller_pool = pool,
        .ops = &ops,
    };
    ztk_udp_server *srv = ztk_udp_server_create(&opts);
    if (!srv || ztk_udp_server_start(srv) != ZTK_OK)
        return NULL;
    printf("[UDP unicast] listening on 0.0.0.0:%u (pollers=%u)\n", (unsigned)ztk_udp_server_port(srv),
           ztk_udp_server_poller_count(srv));
    return srv;
}

int main(void)
{
    ztk_platform_init();
    demo_print_banner("05", "TCP + UDP share ztk_poller_pool");
    demo_udp_banner("05", "same process as above");

    ztk_poller_pool_opts_t popts = {
        .size = 4,
        .prefer_current_thread = 0,
        .thread_priority = ZTK_THREAD_PRIO_NORMAL,
    };
    ztk_poller_pool *pool = ztk_poller_pool_create(&popts);
    if (!pool || ztk_poller_pool_start(pool) != ZTK_OK) {
        fprintf(stderr, "[05] poller_pool start failed\n");
        if (pool)
            ztk_poller_pool_destroy(pool);
        return 1;
    }

    printf("[05] poller_pool started, pollers=%u\n", ztk_poller_pool_size(pool));
    printf("[05] TcpServer and UdpServer share pool (no manual poller_run)\n\n");

    ztk_tcp_server *tcp = start_tcp(pool);
    ztk_udp_server *udp = start_udp(pool);
    if (!tcp || !udp) {
        fprintf(stderr, "[05] start failed (port 554 may need root; use -DDEMO_RTSP_PORT=8554)\n");
        if (tcp)
            ztk_tcp_server_destroy(tcp);
        if (udp)
            ztk_udp_server_destroy(udp);
        ztk_poller_pool_stop(pool);
        ztk_poller_pool_destroy(pool);
        return 1;
    }

    printf("\n[05] test commands:\n");
    printf("  TCP: echo hello | nc 127.0.0.1 %u\n", (unsigned)ztk_tcp_server_port(tcp));
    printf("  UDP: echo hello | nc -u 127.0.0.1 %u\n", (unsigned)ztk_udp_server_port(udp));
    printf("[05] exits in ~300 seconds\n\n");

    demo_run_seconds(&g_stop, 300);

    ztk_udp_server_destroy(udp);
    ztk_tcp_server_destroy(tcp);
    ztk_poller_pool_stop(pool);
    ztk_poller_pool_destroy(pool);
    return 0;
}
