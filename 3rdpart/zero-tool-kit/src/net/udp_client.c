#include "ztk/net/udp_client.h"
#include "ztk/net/socket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZTK_UDP_CLIENT_RECV 65536

struct ztk_udp_client {
    ztk_socket *sock;
    ztk_poller *poller;
    ztk_udp_client_ops_t ops;
    void *user;

    char peer_host[64];
    uint16_t peer_port;
    int has_peer;
    int started;
    uint8_t *recv_buf;
    size_t recv_cap;
};

static int udp_client_ensure_recv_buf(ztk_udp_client *client)
{
    if (!client)
        return 0;
    if (client->recv_buf && client->recv_cap >= ZTK_UDP_CLIENT_RECV)
        return 1;
    uint8_t *p = (uint8_t *)realloc(client->recv_buf, ZTK_UDP_CLIENT_RECV);
    if (!p)
        return 0;
    client->recv_buf = p;
    client->recv_cap = ZTK_UDP_CLIENT_RECV;
    return 1;
}

static void udp_client_on_readable(ztk_socket *sock, void *user)
{
    ztk_udp_client *client = (ztk_udp_client *)user;
    if (!client || !client->ops.on_packet)
        return;
    if (!udp_client_ensure_recv_buf(client))
        return;

    char from_ip[64];
    uint16_t from_port = 0;

    for (;;) {
        ztk_ssize_t n = ztk_socket_recvfrom(sock, client->recv_buf, client->recv_cap, from_ip, sizeof(from_ip),
            &from_port);
        if (n > 0) {
            client->ops.on_packet(client, client->recv_buf, (size_t)n, from_ip, from_port, client->user);
            continue;
        }
        if (n == ZTK_ERR_AGAIN)
            break;
        if (client->ops.on_error)
            client->ops.on_error(client, client->user);
        break;
    }
}

static void udp_client_on_error(ztk_socket *sock, void *user)
{
    (void)sock;
    ztk_udp_client *client = (ztk_udp_client *)user;
    if (client && client->ops.on_error)
        client->ops.on_error(client, client->user);
}

ztk_udp_client *ztk_udp_client_create(const ztk_udp_client_opts_t *opts)
{
    if (!opts || !opts->poller || !opts->ops)
        return NULL;

    ztk_udp_client *client = (ztk_udp_client *)calloc(1, sizeof(*client));
    if (!client)
        return NULL;

    client->sock = ztk_socket_create();
    if (!client->sock) {
        free(client);
        return NULL;
    }

    client->poller = opts->poller;
    client->ops = *opts->ops;
    client->user = opts->user;
    return client;
}

void ztk_udp_client_destroy(ztk_udp_client *client)
{
    if (!client)
        return;
    if (client->started)
        ztk_socket_detach_poller(client->sock);
    ztk_socket_destroy(client->sock);
    free(client->recv_buf);
    free(client);
}

static ztk_err_t udp_client_bind_and_attach(ztk_udp_client *client, const char *local_host, uint16_t local_port,
    int reuse)
{
    if (!client)
        return ZTK_ERR_INVALID;

    if (client->started)
        ztk_socket_detach_poller(client->sock);

    ztk_err_t err = ztk_socket_bind_udp(client->sock, local_host, local_port, reuse);
    if (err != ZTK_OK)
        return err;

    ztk_socket_callbacks_t cb = { udp_client_on_readable, NULL, udp_client_on_error };
    err = ztk_socket_attach_poller(client->sock, client->poller, &cb, client);
    if (err != ZTK_OK)
        return err;

    client->started = 1;
    return ZTK_OK;
}

ztk_err_t ztk_udp_client_bind(ztk_udp_client *client, const char *local_host, uint16_t local_port, int reuse)
{
    if (!client)
        return ZTK_ERR_INVALID;
    client->has_peer = 0;
    client->peer_host[0] = '\0';
    client->peer_port = 0;
    return udp_client_bind_and_attach(client, local_host, local_port, reuse);
}

void ztk_udp_client_set_peer(ztk_udp_client *client, const char *peer_host, uint16_t peer_port)
{
    if (!client || !peer_host || !peer_host[0])
        return;
    snprintf(client->peer_host, sizeof(client->peer_host), "%s", peer_host);
    client->peer_port = peer_port;
    client->has_peer = 1;
}

ztk_err_t ztk_udp_client_start(ztk_udp_client *client, const char *peer_host, uint16_t peer_port,
                                  const char *local_host, uint16_t local_port, int reuse)
{
    if (!client || !peer_host)
        return ZTK_ERR_INVALID;

    ztk_err_t err = udp_client_bind_and_attach(client, local_host, local_port, reuse);
    if (err != ZTK_OK)
        return err;

    ztk_udp_client_set_peer(client, peer_host, peer_port);
    return ZTK_OK;
}

ztk_err_t ztk_udp_client_send(ztk_udp_client *client, const void *data, size_t len)
{
    if (!client || !client->has_peer)
        return ZTK_ERR_STATE;
    return ztk_udp_client_sendto(client, data, len, client->peer_host, client->peer_port);
}

ztk_err_t ztk_udp_client_sendto(ztk_udp_client *client, const void *data, size_t len, const char *ip,
                                  uint16_t port)
{
    if (!client || !client->started || !ip)
        return ZTK_ERR_STATE;
    ztk_ssize_t n = ztk_socket_sendto(client->sock, data, len, ip, port);
    if (n < 0)
        return (ztk_err_t)n;
    return ZTK_OK;
}

uint16_t ztk_udp_client_local_port(const ztk_udp_client *client)
{
    uint16_t port = 0;
    if (client && client->started)
        ztk_socket_get_local(client->sock, NULL, 0, &port);
    return port;
}
