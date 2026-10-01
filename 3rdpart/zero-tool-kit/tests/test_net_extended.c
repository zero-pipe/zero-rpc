#include "ztk/net/tcp_client.h"
#include "ztk/net/udp_server.h"
#include "ztk/net/udp_client.h"
#include "ztk/net/socket.h"
#include "ztk/poller/poller.h"
#include "ztk/poller/poller_pool.h"
#include "test_portable.h"
#include <stdio.h>
#include <string.h>

static volatile int g_done;
static volatile int g_connected;

static void tcp_srv_read(ztk_socket *sock, void *user)
{
    char buf[32];
    ztk_ssize_t n = ztk_socket_recv(sock, buf, sizeof(buf));
    if (n > 0) {
        ztk_socket_send(sock, buf, (size_t)n);
        g_done = 1;
    }
    (void)user;
}

static void tcp_listen_read(ztk_socket *sock, void *user)
{
    ztk_poller *p = (ztk_poller *)user;
    ztk_socket *c = NULL;
    if (ztk_socket_accept(sock, &c) == ZTK_OK && c) {
        ztk_socket_callbacks_t cb = { tcp_srv_read, NULL, NULL };
        ztk_socket_attach_poller(c, p, &cb, NULL);
    }
}

static void on_tcp_connect(ztk_tcp_client *client, void *user)
{
    (void)client;
    (void)user;
    g_connected = 1;
}

static void on_tcp_recv(ztk_tcp_client *client, const void *data, size_t len, void *user)
{
    (void)client;
    (void)user;
    if (len == 3 && memcmp(data, "cli", 3) == 0)
        g_done = 1;
}

static int test_tcp_client_api(void)
{
    ztk_poller *p = ztk_poller_create();
    ztk_socket *listen = ztk_socket_create();
    if (!p || !listen)
        return 1;

    if (ztk_socket_listen(listen, "127.0.0.1", 0, 8) != ZTK_OK)
        return 1;

    char lip[64];
    uint16_t lport = 0;
    ztk_socket_get_local(listen, lip, sizeof(lip), &lport);

    ztk_socket_callbacks_t lcb = { tcp_listen_read, NULL, NULL };
    ztk_socket_attach_poller(listen, p, &lcb, p);

    g_connected = 0;
    g_done = 0;

    ztk_tcp_client_ops_t ops = { on_tcp_connect, on_tcp_recv, NULL };
    ztk_tcp_client_opts_t copts = { .poller = p, .ops = &ops };
    ztk_tcp_client *cli = ztk_tcp_client_create(&copts);
    if (!cli || ztk_tcp_client_connect(cli, lip, lport) != ZTK_OK)
        return 1;

    for (int i = 0; i < 200 && !g_connected; ++i)
        ztk_poller_poll(p, 10);

    if (!g_connected)
        return 1;

    ztk_tcp_client_send(cli, "cli", 3);

    for (int i = 0; i < 200 && !g_done; ++i)
        ztk_poller_poll(p, 10);

    ztk_tcp_client_destroy(cli);
    ztk_socket_destroy(listen);
    ztk_poller_destroy(p);
    return g_done ? 0 : 1;
}

static void on_udp_packet(ztk_udp_server *srv, const char *ip, uint16_t port, const void *data, size_t len,
                          void *user)
{
    (void)user;
    ztk_udp_server_sendto(srv, ip, port, data, len);
    g_done = 1;
}

static volatile int g_udp_pkts;

static void on_udp_multi(ztk_udp_server *srv, const char *ip, uint16_t port, const void *data,
                         size_t len, void *user)
{
    (void)srv;
    (void)ip;
    (void)port;
    (void)data;
    (void)user;
    if (len > 0)
        ++g_udp_pkts;
}

static int platform_has_reuseport(void)
{
#if defined(SO_REUSEPORT)
    return 1;
#else
    return 0;
#endif
}

static int test_udp_reuseport_multi(void)
{
    if (!platform_has_reuseport())
        return 0;

    ztk_poller_pool_opts_t popts = { .size = 2, .thread_priority = ZTK_THREAD_PRIO_NORMAL };
    ztk_poller_pool *pool = ztk_poller_pool_create(&popts);
    if (!pool || ztk_poller_pool_start(pool) != ZTK_OK)
        return 1;

    ztk_udp_server_ops_t sops = { on_udp_multi };
    ztk_udp_server_opts_t sopts = {
        .host = "127.0.0.1",
        .port = 0,
        .reuse = 1,
        .reuse_port = 1,
        .poller_pool = pool,
        .ops = &sops
    };
    ztk_udp_server *srv = ztk_udp_server_create(&sopts);
    if (!srv || ztk_udp_server_start(srv) != ZTK_OK) {
        ztk_poller_pool_stop(pool);
        ztk_poller_pool_destroy(pool);
        return 1;
    }

    if (ztk_udp_server_poller_count(srv) < 2) {
        ztk_udp_server_destroy(srv);
        ztk_poller_pool_stop(pool);
        ztk_poller_pool_destroy(pool);
        return 1;
    }

    char lip[64];
    uint16_t lport = ztk_udp_server_port(srv);
    snprintf(lip, sizeof(lip), "127.0.0.1");

    ztk_socket *cli = ztk_socket_create();
    ztk_socket_bind_udp(cli, "127.0.0.1", 0, 1);

    g_udp_pkts = 0;
    const char *msg = "rp";
    for (int i = 0; i < 32; ++i)
        ztk_socket_sendto(cli, msg, 2, lip, lport);

    for (int t = 0; t < 300 && g_udp_pkts < 32; ++t)
        ztk_test_sleep_ms(10);

    ztk_socket_destroy(cli);
    ztk_udp_server_destroy(srv);
    ztk_poller_pool_stop(pool);
    ztk_poller_pool_destroy(pool);

    return g_udp_pkts == 32 ? 0 : 1;
}

static int test_udp_server_client(void)
{
    ztk_poller *p = ztk_poller_create();
    if (!p)
        return 1;

    ztk_udp_server_ops_t sops = { on_udp_packet };
    ztk_udp_server_opts_t sopts = {
        .host = "127.0.0.1",
        .port = 0,
        .reuse = 1,
        .poller = p,
        .ops = &sops
    };
    ztk_udp_server *srv = ztk_udp_server_create(&sopts);
    if (!srv || ztk_udp_server_start(srv) != ZTK_OK)
        return 1;

    char lip[64];
    uint16_t lport = ztk_udp_server_port(srv);
    snprintf(lip, sizeof(lip), "127.0.0.1");

    ztk_udp_client_ops_t cops = { NULL, NULL };
    ztk_udp_client_opts_t copts = { .poller = p, .ops = &cops };
    ztk_udp_client *cli = ztk_udp_client_create(&copts);
    if (!cli || ztk_udp_client_start(cli, lip, lport, "127.0.0.1", 0, 1) != ZTK_OK)
        return 1;

    g_done = 0;
    ztk_udp_client_send(cli, "udp", 3);

    for (int i = 0; i < 100 && !g_done; ++i)
        ztk_poller_poll(p, 20);

    ztk_udp_client_destroy(cli);
    ztk_udp_server_destroy(srv);
    ztk_poller_destroy(p);
    return g_done ? 0 : 1;
}

static int test_multicast(void)
{
    const char *group = "239.255.0.1";
    const uint16_t port = 37020;

    ztk_poller *p = ztk_poller_create();
    ztk_socket *recv = ztk_socket_create();
    ztk_socket *send = ztk_socket_create();
    if (!p || !recv || !send)
        return 1;

    if (ztk_socket_bind_udp(recv, "0.0.0.0", port, 1) != ZTK_OK)
        return 1;
    if (ztk_socket_join_multicast(recv, group, NULL) != ZTK_OK)
        return 1;
    if (ztk_socket_set_multicast_loop(recv, 1) != ZTK_OK)
        return 1;

    if (ztk_socket_bind_udp(send, "0.0.0.0", 0, 1) != ZTK_OK)
        return 1;
    if (ztk_socket_set_multicast_ttl(send, 1) != ZTK_OK)
        return 1;

    const char *msg = "mcast";
    if (ztk_socket_sendto(send, msg, 5, group, port) < 0)
        return 1;

    ztk_test_sleep_ms(50);

    char buf[16];
    ztk_ssize_t n = ztk_socket_recvfrom(recv, buf, sizeof(buf), NULL, 0, NULL);
    if (n != 5 || memcmp(buf, msg, 5) != 0) {
        ztk_socket_destroy(recv);
        ztk_socket_destroy(send);
        ztk_poller_destroy(p);
        return 1;
    }

    ztk_socket_leave_multicast(recv, group, NULL);
    ztk_socket_destroy(recv);
    ztk_socket_destroy(send);
    ztk_poller_destroy(p);
    return 0;
}

static int test_broadcast(void)
{
    ztk_socket *recv = ztk_socket_create();
    ztk_socket *send = ztk_socket_create();
    if (!recv || !send)
        return 1;

    if (ztk_socket_bind_udp(recv, "0.0.0.0", 0, 1) != ZTK_OK)
        return 1;

    char lip[64];
    uint16_t lport = 0;
    if (ztk_socket_get_local(recv, lip, sizeof(lip), &lport) != ZTK_OK)
        return 1;

    if (ztk_socket_bind_udp(send, "0.0.0.0", 0, 1) != ZTK_OK)
        return 1;
    if (ztk_socket_set_broadcast(send, 1) != ZTK_OK)
        return 1;

    const char *msg = "bcast";
    if (ztk_socket_sendto(send, msg, 5, "255.255.255.255", lport) < 0)
        return 1;

    ztk_test_sleep_ms(50);

    char buf[16];
    char rip[64];
    uint16_t rport = 0;
    ztk_ssize_t n = ztk_socket_recvfrom(recv, buf, sizeof(buf), rip, sizeof(rip), &rport);
    if (n != 5 || memcmp(buf, msg, 5) != 0) {
        ztk_socket_destroy(recv);
        ztk_socket_destroy(send);
        return 1;
    }

    ztk_socket_destroy(recv);
    ztk_socket_destroy(send);
    return 0;
}

int main(void)
{
    if (test_tcp_client_api() != 0) {
        fprintf(stderr, "tcp_client failed\n");
        return 1;
    }
    if (test_udp_server_client() != 0) {
        fprintf(stderr, "udp server/client failed\n");
        return 1;
    }
    if (test_udp_reuseport_multi() != 0) {
        fprintf(stderr, "udp reuseport multi-poller failed\n");
        return 1;
    }
    if (test_multicast() != 0) {
        fprintf(stderr, "multicast failed\n");
        return 1;
    }
    if (test_broadcast() != 0) {
        fprintf(stderr, "broadcast failed\n");
        return 1;
    }
    printf("test_net_extended ok\n");
    return 0;
}
