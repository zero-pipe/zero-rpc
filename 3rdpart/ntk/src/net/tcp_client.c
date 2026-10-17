#include "net/tcp_client.h"
#include "net/socket.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NTK_TCP_CLIENT_RECV 4096

struct ntk_tcp_client {
    ntk_socket *sock;
    ntk_poller *poller;
    ntk_tcp_client_ops_t ops;
    void *user;
    int connected;
    int closing;
};

static void client_on_error(ntk_socket *sock, void *user);
static void client_on_readable(ntk_socket *sock, void *user);
static void client_on_writable(ntk_socket *sock, void *user);

static void fire_error(ntk_tcp_client *client)
{
    if (!client || client->closing)
        return;
    client->closing = 1;
    if (client->ops.on_error)
        client->ops.on_error(client, client->user);
    ntk_socket_detach_poller(client->sock);
}

static void client_on_writable(ntk_socket *sock, void *user)
{
    ntk_tcp_client *client = (ntk_tcp_client *)user;
    if (!client || client->closing || client->connected)
        return;

    ntk_err_t err = ntk_socket_check_connect(sock);
    if (err == NTK_OK) {
        client->connected = 1;
        if (client->ops.on_connect)
            client->ops.on_connect(client, client->user);
        return;
    }
    if (err != NTK_ERR_AGAIN)
        fire_error(client);
}

static void client_on_readable(ntk_socket *sock, void *user)
{
    ntk_tcp_client *client = (ntk_tcp_client *)user;
    if (!client || client->closing)
        return;

    if (!client->connected) {
        client_on_writable(sock, user);
        if (!client->connected || client->closing)
            return;
    }

    char buf[NTK_TCP_CLIENT_RECV];
    ntk_ssize_t n = ntk_socket_recv(sock, buf, sizeof(buf));
    if (n > 0) {
        if (client->ops.on_recv)
            client->ops.on_recv(client, buf, (size_t)n, client->user);
        return;
    }
    if (n == 0 || (n < 0 && n != NTK_ERR_AGAIN))
        fire_error(client);
}

static void client_on_error(ntk_socket *sock, void *user)
{
    (void)sock;
    fire_error((ntk_tcp_client *)user);
}

ntk_tcp_client *ntk_tcp_client_create(const ntk_tcp_client_opts_t *opts)
{
    if (!opts || !opts->poller || !opts->ops)
        return NULL;

    ntk_tcp_client *client = (ntk_tcp_client *)calloc(1, sizeof(*client));
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

void ntk_tcp_client_destroy(ntk_tcp_client *client)
{
    if (!client)
        return;
    ntk_tcp_client_close(client);
    ntk_socket_destroy(client->sock);
    free(client);
}

ntk_err_t ntk_tcp_client_connect(ntk_tcp_client *client, const char *host, uint16_t port)
{
    if (!client || client->closing)
        return NTK_ERR_INVALID;

    ntk_socket_detach_poller(client->sock);
    client->connected = 0;

    int pending = 0;
    ntk_err_t err = ntk_socket_connect(client->sock, host, port, &pending);
    if (err != NTK_OK)
        return err;

    ntk_socket_callbacks_t cb = { client_on_readable, client_on_writable, client_on_error };
    err = ntk_socket_attach_poller(client->sock, client->poller, &cb, client);
    if (err != NTK_OK)
        return err;

    if (!pending) {
        client->connected = 1;
        if (client->ops.on_connect)
            client->ops.on_connect(client, client->user);
    }
    return NTK_OK;
}

int ntk_tcp_client_is_connected(const ntk_tcp_client *client)
{
    return client && client->connected && !client->closing;
}

ntk_socket *ntk_tcp_client_socket(ntk_tcp_client *client)
{
    return client ? client->sock : NULL;
}

ntk_ssize_t ntk_tcp_client_send(ntk_tcp_client *client, const void *data, size_t len)
{
    if (!client || !client->connected || client->closing || !data || len == 0)
        return (ntk_ssize_t)NTK_ERR_STATE;
    /* 单次非阻塞写：返回已写字节数；EAGAIN(0 写) 时返回 NTK_ERR_AGAIN。
     * 调用方必须依据返回值推进，切勿在未消费返回值时整段重发。 */
    return ntk_socket_send(client->sock, data, len);
}

void ntk_tcp_client_close(ntk_tcp_client *client)
{
    if (!client)
        return;
    /* fire_error 会置 closing=1；此处必须始终重建 socket，不能因 closing 直接 return */
    client->closing = 1;
    ntk_socket_detach_poller(client->sock);
    ntk_socket_destroy(client->sock);
    client->sock = ntk_socket_create();
    client->connected = 0;
    client->closing = 0;
}
