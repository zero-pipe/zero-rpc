/**
 * [04] 单播 — 发送端
 * API: ztk_udp_client
 *
 * 运行: ./demo_04_udp_unicast_send [目标IP] [端口]
 * 默认: 127.0.0.1 DEMO_UDP_UNICAST_PORT
 */
#include "ztk/net/udp_client.h"
#include "ztk/poller/poller.h"
#include "ztk/thread/thread.h"
#include "demo_udp_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int g_stop;

static void poller_entry(void *arg)
{
    ztk_poller_run((ztk_poller *)arg, &g_stop);
}

int main(int argc, char **argv)
{
    const char *peer_ip = "127.0.0.1";
    uint16_t peer_port = DEMO_UDP_UNICAST_PORT;

    if (argc >= 2)
        peer_ip = argv[1];
    if (argc >= 3)
        peer_port = (uint16_t)atoi(argv[2]);

    ztk_platform_init();
    demo_udp_banner("07", "unicast send (ztk_udp_client)");

    ztk_poller *p = ztk_poller_create();
    if (!p)
        return 1;

    ztk_udp_client_ops_t cops = { NULL, NULL };
    ztk_udp_client_opts_t copts = { .poller = p, .ops = &cops };
    ztk_udp_client *cli = ztk_udp_client_create(&copts);
    if (!cli) {
        ztk_poller_destroy(p);
        return 1;
    }

    if (ztk_udp_client_start(cli, peer_ip, peer_port, "0.0.0.0", 0, 1) != ZTK_OK) {
        fprintf(stderr, "[07] udp_client start failed\n");
        ztk_udp_client_destroy(cli);
        ztk_poller_destroy(p);
        return 1;
    }

    ztk_thread *t = ztk_thread_create(poller_entry, p);
    if (!t) {
        ztk_udp_client_destroy(cli);
        ztk_poller_destroy(p);
        return 1;
    }

    printf("[07] send to %s:%u every 2s, exits in ~60 seconds\n", peer_ip, (unsigned)peer_port);

    for (int i = 0; i < 30 && !g_stop; ++i) {
        char msg[64];
        snprintf(msg, sizeof(msg), "unicast #%d", i + 1);
        demo_udp_print_send("UDP unicast", peer_ip, peer_port, msg);
        ztk_udp_client_send(cli, msg, strlen(msg));
        ztk_sleep_ms(2000);
    }

    g_stop = 1;
    ztk_thread_join(t);
    ztk_thread_destroy(t);
    ztk_udp_client_destroy(cli);
    ztk_poller_destroy(p);
    return 0;
}
