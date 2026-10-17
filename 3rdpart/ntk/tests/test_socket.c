#include "ztk/net/socket.h"
#include "ztk/poller/poller.h"
#include <stdio.h>
#include <string.h>
#include "test_portable.h"

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

static int test_tcp_sync(void)
{
    ztk_socket *server = ztk_socket_create();
    ztk_socket *client = ztk_socket_create();
    if (!server || !client)
        return 1;

    if (ztk_socket_listen(server, "127.0.0.1", 0, 64) != ZTK_OK) {
        fprintf(stderr, "listen failed\n");
        return 1;
    }

    char lip[64];
    uint16_t lport = 0;
    if (ztk_socket_get_local(server, lip, sizeof(lip), &lport) != ZTK_OK || lport == 0) {
        fprintf(stderr, "get_local failed\n");
        return 1;
    }

    int pending = 0;
    if (ztk_socket_connect(client, lip, lport, &pending) != ZTK_OK) {
        fprintf(stderr, "connect failed\n");
        return 1;
    }
    if (pending && wait_connect(client, 3000) != ZTK_OK) {
        fprintf(stderr, "connect wait failed\n");
        return 1;
    }

    ztk_socket *accepted = NULL;
    if (ztk_socket_accept(server, &accepted) != ZTK_OK || !accepted) {
        fprintf(stderr, "accept failed\n");
        return 1;
    }

    const char *msg = "ping";
    char buf[16];
    if (ztk_socket_send(client, msg, strlen(msg)) < 0) {
        fprintf(stderr, "client send failed\n");
        return 1;
    }

    ztk_ssize_t n = ztk_socket_recv(accepted, buf, sizeof(buf) - 1);
    if (n != (ztk_ssize_t)strlen(msg) || memcmp(buf, msg, (size_t)n) != 0) {
        fprintf(stderr, "server recv failed n=%zd\n", n);
        return 1;
    }

    if (ztk_socket_send(accepted, msg, strlen(msg)) < 0)
        return 1;

    n = ztk_socket_recv(client, buf, sizeof(buf) - 1);
    if (n != (ztk_ssize_t)strlen(msg) || memcmp(buf, msg, (size_t)n) != 0) {
        fprintf(stderr, "client recv failed\n");
        return 1;
    }

    ztk_socket_destroy(accepted);
    ztk_socket_destroy(client);
    ztk_socket_destroy(server);
    return 0;
}

static int test_udp(void)
{
    ztk_socket *a = ztk_socket_create();
    ztk_socket *b = ztk_socket_create();
    if (!a || !b)
        return 1;

    if (ztk_socket_bind_udp(a, "127.0.0.1", 0, 1) != ZTK_OK)
        return 1;
    if (ztk_socket_bind_udp(b, "127.0.0.1", 0, 1) != ZTK_OK)
        return 1;

    char ip[64];
    uint16_t port = 0;
    if (ztk_socket_get_local(a, ip, sizeof(ip), &port) != ZTK_OK)
        return 1;

    const char *msg = "udp";
    if (ztk_socket_sendto(b, msg, 3, ip, port) < 0)
        return 1;

    char buf[16];
    char rip[64];
    uint16_t rport = 0;
    ztk_ssize_t n = ztk_socket_recvfrom(a, buf, sizeof(buf), rip, sizeof(rip), &rport);
    if (n != 3 || memcmp(buf, msg, 3) != 0)
        return 1;

    ztk_socket_destroy(a);
    ztk_socket_destroy(b);
    return 0;
}

static volatile int g_poller_done;

static void on_srv_read(ztk_socket *sock, void *user)
{
    (void)user;
    char buf[32];
    ztk_ssize_t n = ztk_socket_recv(sock, buf, sizeof(buf));
    if (n > 0) {
        ztk_socket_send(sock, buf, (size_t)n);
        g_poller_done = 1;
    }
}

static void on_listen_read(ztk_socket *sock, void *user)
{
    ztk_poller *poller = (ztk_poller *)user;
    ztk_socket *client = NULL;
    if (ztk_socket_accept(sock, &client) != ZTK_OK || !client)
        return;

    ztk_socket_callbacks_t cb = { on_srv_read, NULL, NULL };
    ztk_socket_attach_poller(client, poller, &cb, NULL);
}

static int test_tcp_poller(void)
{
    ztk_poller *poller = ztk_poller_create();
    ztk_socket *server = ztk_socket_create();
    ztk_socket *client = ztk_socket_create();
    if (!poller || !server || !client)
        return 1;

    if (ztk_socket_listen(server, "127.0.0.1", 0, 64) != ZTK_OK)
        return 1;

    char lip[64];
    uint16_t lport = 0;
    ztk_socket_get_local(server, lip, sizeof(lip), &lport);

    ztk_socket_callbacks_t lcb = { on_listen_read, NULL, NULL };
    ztk_socket_attach_poller(server, poller, &lcb, poller);

    int pending = 0;
    if (ztk_socket_connect(client, lip, lport, &pending) != ZTK_OK)
        return 1;
    if (pending && wait_connect(client, 3000) != ZTK_OK)
        return 1;

    const char *msg = "p";
    ztk_socket_send(client, msg, 1);

    for (int i = 0; i < 100 && !g_poller_done; ++i)
        ztk_poller_poll(poller, 50);

    char buf[8] = {0};
    ztk_socket_recv(client, buf, sizeof(buf));

    ztk_socket_destroy(client);
    ztk_socket_destroy(server);
    ztk_poller_destroy(poller);

    return (buf[0] == 'p') ? 0 : 1;
}

int main(void)
{
    if (test_tcp_sync() != 0) {
        fprintf(stderr, "test_tcp_sync failed\n");
        return 1;
    }
    if (test_udp() != 0) {
        fprintf(stderr, "test_udp failed\n");
        return 1;
    }
    g_poller_done = 0;
    if (test_tcp_poller() != 0) {
        fprintf(stderr, "test_tcp_poller failed\n");
        return 1;
    }
    printf("test_socket ok\n");
    return 0;
}
