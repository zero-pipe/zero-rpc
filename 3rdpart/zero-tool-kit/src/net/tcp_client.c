#include "ztk/net/tcp_client.h"
#include "ztk/net/socket.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ZTK_TCP_CLIENT_RECV 4096

struct ztk_tcp_client {
    ztk_socket *sock;
    ztk_poller *poller;
    ztk_tcp_client_ops_t ops;
    void *user;
    int connected;
    int closing;
};

static void client_on_error(ztk_socket *sock, void *user);
static void client_on_readable(ztk_socket *sock, void *user);
static void client_on_writable(ztk_socket *sock, void *user);

static void fire_error(ztk_tcp_client *client)
{
    if (!client || client->closing)
        return;
    client->closing = 1;
    if (client->ops.on_error)
        client->ops.on_error(client, client->user);
    ztk_socket_detach_poller(client->sock);
}

static void client_on_writable(ztk_socket *sock, void *user)
{
    ztk_tcp_client *client = (ztk_tcp_client *)user;
    if (!client || client->closing || client->connected)
        return;

    ztk_err_t err = ztk_socket_check_connect(sock);
    if (err == ZTK_OK) {
        client->connected = 1;
        if (client->ops.on_connect)
            client->ops.on_connect(client, client->user);
        return;
    }
    if (err != ZTK_ERR_AGAIN)
        fire_error(client);
}

static void client_on_readable(ztk_socket *sock, void *user)
{
    ztk_tcp_client *client = (ztk_tcp_client *)user;
    if (!client || client->closing)
        return;

    if (!client->connected) {
        client_on_writable(sock, user);
        if (!client->connected || client->closing)
            return;
    }

    char buf[ZTK_TCP_CLIENT_RECV];
    ztk_ssize_t n = ztk_socket_recv(sock, buf, sizeof(buf));
    if (n > 0) {
        if (client->ops.on_recv)
            client->ops.on_recv(client, buf, (size_t)n, client->user);
        return;
    }
    if (n == 0 || (n < 0 && n != ZTK_ERR_AGAIN))
        fire_error(client);
}

static void client_on_error(ztk_socket *sock, void *user)
{
    (void)sock;
    fire_error((ztk_tcp_client *)user);
}

ztk_tcp_client *ztk_tcp_client_create(const ztk_tcp_client_opts_t *opts)
{
    if (!opts || !opts->poller || !opts->ops)
        return NULL;

    ztk_tcp_client *client = (ztk_tcp_client *)calloc(1, sizeof(*client));
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

void ztk_tcp_client_destroy(ztk_tcp_client *client)
{
    if (!client)
        return;
    ztk_tcp_client_close(client);
    ztk_socket_destroy(client->sock);
    free(client);
}

ztk_err_t ztk_tcp_client_connect(ztk_tcp_client *client, const char *host, uint16_t port)
{
    if (!client || client->closing)
        return ZTK_ERR_INVALID;

    ztk_socket_detach_poller(client->sock);
    client->connected = 0;

    int pending = 0;
    ztk_err_t err = ztk_socket_connect(client->sock, host, port, &pending);
    if (err != ZTK_OK)
        return err;

    ztk_socket_callbacks_t cb = { client_on_readable, client_on_writable, client_on_error };
    err = ztk_socket_attach_poller(client->sock, client->poller, &cb, client);
    if (err != ZTK_OK)
        return err;

    if (!pending) {
        client->connected = 1;
        if (client->ops.on_connect)
            client->ops.on_connect(client, client->user);
    }
    return ZTK_OK;
}

int ztk_tcp_client_is_connected(const ztk_tcp_client *client)
{
    return client && client->connected && !client->closing;
}

ztk_socket *ztk_tcp_client_socket(ztk_tcp_client *client)
{
    return client ? client->sock : NULL;
}

ztk_ssize_t ztk_tcp_client_send(ztk_tcp_client *client, const void *data, size_t len)
{
    if (!client || !client->connected || client->closing || !data || len == 0)
        return (ztk_ssize_t)ZTK_ERR_STATE;
    /* 单次非阻塞写：返回已写字节数；EAGAIN(0 写) 时返回 ZTK_ERR_AGAIN。
     * 调用方必须依据返回值推进，切勿在未消费返回值时整段重发。 */
    return ztk_socket_send(client->sock, data, len);
}

void ztk_tcp_client_close(ztk_tcp_client *client)
{
    if (!client)
        return;
    /* fire_error 会置 closing=1；此处必须始终重建 socket，不能因 closing 直接 return */
    client->closing = 1;
    ztk_socket_detach_poller(client->sock);
    ztk_socket_destroy(client->sock);
    client->sock = ztk_socket_create();
    client->connected = 0;
    client->closing = 0;
}
