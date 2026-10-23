#include "net/udp_client.h"
#include "net/socket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NTK_UDP_CLIENT_RECV 65536

struct ntk_udp_client {
    ntk_socket *sock;
    ntk_poller *poller;
    ntk_udp_client_ops_t ops;
    void *user;

    char peer_host[64];
    uint16_t peer_port;
    int has_peer;
    int started;
    uint8_t *recv_buf;
    size_t recv_cap;
};

static int udp_client_ensure_recv_buf(ntk_udp_client *client)
{
    if (!client)
        return 0;
    if (client->recv_buf && client->recv_cap >= NTK_UDP_CLIENT_RECV)
        return 1;
    uint8_t *p = (uint8_t *)realloc(client->recv_buf, NTK_UDP_CLIENT_RECV);
    if (!p)
        return 0;
    client->recv_buf = p;
    client->recv_cap = NTK_UDP_CLIENT_RECV;
    return 1;
}

static void udp_client_on_readable(ntk_socket *sock, void *user)
{
    ntk_udp_client *client = (ntk_udp_client *)user;
    if (!client || !client->ops.on_packet)
        return;
    if (!udp_client_ensure_recv_buf(client))
        return;

    char from_ip[64];
    uint16_t from_port = 0;

    for (;;) {
        ntk_ssize_t n = ntk_socket_recvfrom(sock, client->recv_buf, client->recv_cap, from_ip, sizeof(from_ip),
            &from_port);
        if (n > 0) {
            client->ops.on_packet(client, client->recv_buf, (size_t)n, from_ip, from_port, client->user);
            continue;
        }
        if (n == NTK_ERR_AGAIN)
            break;
        if (client->ops.on_error)
            client->ops.on_error(client, client->user);
        break;
    }
}

static void udp_client_on_error(ntk_socket *sock, void *user)
{
    (void)sock;
    ntk_udp_client *client = (ntk_udp_client *)user;
    if (client && client->ops.on_error)
        client->ops.on_error(client, client->user);
}

ntk_udp_client *ntk_udp_client_create(const ntk_udp_client_opts_t *opts)
{
    if (!opts || !opts->poller || !opts->ops)
        return NULL;

    ntk_udp_client *client = (ntk_udp_client *)calloc(1, sizeof(*client));
    if (!client)
        return NULL;

    client->sock = ntk_socket_create();
    if (!client->sock) {
        free(client);
        return NULL;
    }

    client->poller = opts->poller;
    client->ops = *opts->ops;
    client->user = opts->user;
    return client;
}

void ntk_udp_client_destroy(ntk_udp_client *client)
{
    if (!client)
        return;
    if (client->started)
        ntk_socket_detach_poller(client->sock);
    ntk_socket_destroy(client->sock);
    free(client->recv_buf);
    free(client);
}

static ntk_err_t udp_client_bind_and_attach(ntk_udp_client *client, const char *local_host, uint16_t local_port,
    int reuse)
{
    if (!client)
        return NTK_ERR_INVALID;

    if (client->started)
        ntk_socket_detach_poller(client->sock);

    ntk_err_t err = ntk_socket_bind_udp(client->sock, local_host, local_port, reuse);
    if (err != NTK_OK)
        return err;

    ntk_socket_callbacks_t cb = { udp_client_on_readable, NULL, udp_client_on_error };
    err = ntk_socket_attach_poller(client->sock, client->poller, &cb, client);
    if (err != NTK_OK)
        return err;

    client->started = 1;
    return NTK_OK;
}

ntk_err_t ntk_udp_client_bind(ntk_udp_client *client, const char *local_host, uint16_t local_port, int reuse)
{
    if (!client)
        return NTK_ERR_INVALID;
    client->has_peer = 0;
    client->peer_host[0] = '\0';
    client->peer_port = 0;
    return udp_client_bind_and_attach(client, local_host, local_port, reuse);
}

void ntk_udp_client_set_peer(ntk_udp_client *client, const char *peer_host, uint16_t peer_port)
{
    if (!client || !peer_host || !peer_host[0])
        return;
    snprintf(client->peer_host, sizeof(client->peer_host), "%s", peer_host);
    client->peer_port = peer_port;
    client->has_peer = 1;
}

ntk_err_t ntk_udp_client_start(ntk_udp_client *client, const char *peer_host, uint16_t peer_port,
                                  const char *local_host, uint16_t local_port, int reuse)
{
    if (!client || !peer_host)
        return NTK_ERR_INVALID;

    ntk_err_t err = udp_client_bind_and_attach(client, local_host, local_port, reuse);
    if (err != NTK_OK)
        return err;

    ntk_udp_client_set_peer(client, peer_host, peer_port);
    return NTK_OK;
}

ntk_err_t ntk_udp_client_send(ntk_udp_client *client, const void *data, size_t len)
{
    if (!client || !client->has_peer)
        return NTK_ERR_STATE;
    return ntk_udp_client_sendto(client, data, len, client->peer_host, client->peer_port);
}

ntk_err_t ntk_udp_client_sendto(ntk_udp_client *client, const void *data, size_t len, const char *ip,
                                  uint16_t port)
{
    if (!client || !client->started || !ip)
        return NTK_ERR_STATE;
    ntk_ssize_t n = ntk_socket_sendto(client->sock, data, len, ip, port);
    if (n < 0)
        return (ntk_err_t)n;
    return NTK_OK;
}

uint16_t ntk_udp_client_local_port(const ntk_udp_client *client)
{
    uint16_t port = 0;
    if (client && client->started)
        ntk_socket_get_local(client->sock, NULL, 0, &port);
    return port;
}
