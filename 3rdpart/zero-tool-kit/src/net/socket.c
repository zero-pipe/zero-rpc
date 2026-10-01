#include "ztk/net/socket.h"
#include "ztk/poller/poller.h"
#include "internal/sock_platform.h"
#include <stdlib.h>
#include <string.h>

struct ztk_socket {
    int fd;
    ztk_sock_type_t type;
    ztk_poller *poller;
    ztk_socket_callbacks_t cb;
    void *user;
    unsigned poll_events;
    int attached;
    int connect_pending;
};

static int s_sockplat_inited;

static ztk_err_t ensure_sockplat(void)
{
    if (s_sockplat_inited)
        return ZTK_OK;
    ztk_err_t err = ztk_sockplat_init();
    if (err == ZTK_OK)
        s_sockplat_inited = 1;
    return err;
}

static void close_fd(ztk_socket *sock)
{
    if (!sock || sock->fd < 0)
        return;
    ztk_sockplat_close(sock->fd);
    sock->fd = -1;
}

static unsigned default_poll_events(const ztk_socket *sock)
{
    unsigned ev = ZTK_POLL_IN | ZTK_POLL_ERR | ZTK_POLL_HUP;
    if (sock->connect_pending)
        ev |= ZTK_POLL_OUT;
    return ev;
}

static void socket_poller_cb(int fd, unsigned events, void *user)
{
    ztk_socket *sock = (ztk_socket *)user;
    if (!sock || sock->fd != fd)
        return;

    if (events & (ZTK_POLL_ERR | ZTK_POLL_HUP)) {
        if (sock->cb.on_error)
            sock->cb.on_error(sock, sock->user);
        return;
    }

    if ((events & ZTK_POLL_OUT) && sock->connect_pending) {
        if (ztk_socket_check_connect(sock) == ZTK_OK) {
            sock->connect_pending = 0;
            sock->type = ZTK_SOCK_TCP_CONNECTED;
            if (sock->attached)
                ztk_socket_mod_events(sock, default_poll_events(sock));
        } else if (sock->cb.on_error) {
            sock->cb.on_error(sock, sock->user);
            return;
        }
    }

    if (events & ZTK_POLL_OUT) {
        if (sock->cb.on_writable)
            sock->cb.on_writable(sock, sock->user);
    }

    if (events & ZTK_POLL_IN) {
        if (sock->cb.on_readable)
            sock->cb.on_readable(sock, sock->user);
    }
}

ztk_socket *ztk_socket_create(void)
{
    ztk_socket *sock = (ztk_socket *)calloc(1, sizeof(*sock));
    if (sock)
        sock->fd = -1;
    return sock;
}

void ztk_socket_destroy(ztk_socket *sock)
{
    if (!sock)
        return;
    ztk_socket_detach_poller(sock);
    close_fd(sock);
    free(sock);
}

ztk_err_t ztk_socket_adopt(ztk_socket *sock, int fd, ztk_sock_type_t type)
{
    if (!sock || fd < 0)
        return ZTK_ERR_INVALID;
    if (ensure_sockplat() != ZTK_OK)
        return ZTK_ERR_PLATFORM;

    ztk_socket_detach_poller(sock);
    close_fd(sock);

    ztk_err_t err = ZTK_OK;
    switch (type) {
    case ZTK_SOCK_TCP_LISTEN:
        err = ztk_sockplat_apply_tcp_server_opts(fd);
        break;
    case ZTK_SOCK_TCP_CLIENT:
    case ZTK_SOCK_TCP_CONNECTED:
        err = ztk_sockplat_apply_tcp_client_opts(fd);
        break;
    case ZTK_SOCK_UDP:
        err = ztk_sockplat_apply_udp_opts(fd);
        break;
    default:
        return ZTK_ERR_INVALID;
    }
    if (err != ZTK_OK)
        return err;

    sock->fd = fd;
    sock->type = type;
    sock->connect_pending = 0;
    return ZTK_OK;
}

int ztk_socket_fd(const ztk_socket *sock)
{
    return sock ? sock->fd : -1;
}

ztk_sock_type_t ztk_socket_type(const ztk_socket *sock)
{
    return sock ? sock->type : ZTK_SOCK_NONE;
}

ztk_err_t ztk_socket_listen(ztk_socket *sock, const char *host, uint16_t port, int backlog)
{
    if (!sock)
        return ZTK_ERR_INVALID;
    if (ensure_sockplat() != ZTK_OK)
        return ZTK_ERR_PLATFORM;

    ztk_socket_detach_poller(sock);
    close_fd(sock);

    int fd = -1;
    ztk_err_t err = ztk_sockplat_listen(&fd, host, port, backlog);
    if (err != ZTK_OK)
        return err;

    sock->fd = fd;
    sock->type = ZTK_SOCK_TCP_LISTEN;
    sock->connect_pending = 0;
    return ZTK_OK;
}

ztk_err_t ztk_socket_bind_udp_ex(ztk_socket *sock, const char *host, uint16_t port, int reuse_addr,
                                   int reuse_port)
{
    if (!sock)
        return ZTK_ERR_INVALID;
    if (ensure_sockplat() != ZTK_OK)
        return ZTK_ERR_PLATFORM;

    ztk_socket_detach_poller(sock);
    close_fd(sock);

    int fd = -1;
    ztk_err_t err = ztk_sockplat_bind_udp(&fd, host, port, reuse_addr, reuse_port);
    if (err != ZTK_OK)
        return err;

    sock->fd = fd;
    sock->type = ZTK_SOCK_UDP;
    sock->connect_pending = 0;
    return ZTK_OK;
}

ztk_err_t ztk_socket_bind_udp(ztk_socket *sock, const char *host, uint16_t port, int reuse)
{
    return ztk_socket_bind_udp_ex(sock, host, port, reuse, 0);
}

ztk_err_t ztk_socket_connect(ztk_socket *sock, const char *host, uint16_t port, int *in_progress)
{
    if (!sock || !host)
        return ZTK_ERR_INVALID;
    if (ensure_sockplat() != ZTK_OK)
        return ZTK_ERR_PLATFORM;

    ztk_socket_detach_poller(sock);
    close_fd(sock);

    int fd = -1;
    int pending = 0;
    ztk_err_t err = ztk_sockplat_connect(&fd, host, port, &pending);
    if (err != ZTK_OK)
        return err;

    sock->fd = fd;
    sock->type = pending ? ZTK_SOCK_TCP_CLIENT : ZTK_SOCK_TCP_CONNECTED;
    sock->connect_pending = pending ? 1 : 0;
    if (in_progress)
        *in_progress = pending;
    return ZTK_OK;
}

ztk_err_t ztk_socket_check_connect(ztk_socket *sock)
{
    if (!sock || sock->fd < 0)
        return ZTK_ERR_INVALID;
    if (!sock->connect_pending)
        return ZTK_OK;
    ztk_err_t err = ztk_sockplat_check_connect(sock->fd);
    if (err == ZTK_OK) {
        sock->connect_pending = 0;
        sock->type = ZTK_SOCK_TCP_CONNECTED;
    }
    return err;
}

ztk_err_t ztk_socket_accept(ztk_socket *listen_sock, ztk_socket **out_client)
{
    if (!listen_sock || listen_sock->type != ZTK_SOCK_TCP_LISTEN || !out_client)
        return ZTK_ERR_INVALID;

    int cfd = -1;
    ztk_err_t err = ztk_sockplat_accept(listen_sock->fd, &cfd);
    if (err == ZTK_ERR_AGAIN)
        return ZTK_ERR_AGAIN;
    if (err != ZTK_OK)
        return err;

    ztk_socket *client = ztk_socket_create();
    if (!client) {
        ztk_sockplat_close(cfd);
        return ZTK_ERR_NOMEM;
    }
    client->fd = cfd;
    client->type = ZTK_SOCK_TCP_CONNECTED;
    client->connect_pending = 0;
    *out_client = client;
    return ZTK_OK;
}

ztk_ssize_t ztk_socket_send(ztk_socket *sock, const void *data, size_t len)
{
    if (!sock || sock->fd < 0)
        return (ztk_ssize_t)ZTK_ERR_INVALID;
    return ztk_sockplat_send(sock->fd, data, len);
}

ztk_ssize_t ztk_socket_sendv(ztk_socket *sock, const ztk_socket_iov *iov, unsigned count)
{
    ztk_sock_iov plat[ZTK_SOCK_IOV_MAX];
    unsigned i;

    if (!sock || sock->fd < 0)
        return (ztk_ssize_t)ZTK_ERR_INVALID;
    if (!iov || count == 0)
        return (ztk_ssize_t)ZTK_ERR_INVALID;
    if (count > ZTK_SOCKET_IOV_MAX)
        count = ZTK_SOCKET_IOV_MAX;

    for (i = 0; i < count; ++i) {
        plat[i].base = iov[i].base;
        plat[i].len = iov[i].len;
    }
    return ztk_sockplat_sendv(sock->fd, plat, count);
}

ztk_err_t ztk_socket_set_bbr(ztk_socket *sock, int on)
{
    if (!sock || sock->fd < 0)
        return ZTK_ERR_INVALID;
    if (sock->type != ZTK_SOCK_TCP_CONNECTED && sock->type != ZTK_SOCK_TCP_CLIENT)
        return ZTK_ERR_INVALID;
    return ztk_sockplat_set_tcp_bbr(sock->fd, on);
}

ztk_ssize_t ztk_socket_recv(ztk_socket *sock, void *buf, size_t len)
{
    if (!sock || sock->fd < 0)
        return (ztk_ssize_t)ZTK_ERR_INVALID;
    return ztk_sockplat_recv(sock->fd, buf, len);
}

ztk_ssize_t ztk_socket_sendto(ztk_socket *sock, const void *data, size_t len, const char *ip, uint16_t port)
{
    if (!sock || sock->type != ZTK_SOCK_UDP)
        return (ztk_ssize_t)ZTK_ERR_INVALID;
    return ztk_sockplat_sendto(sock->fd, data, len, ip, port);
}

ztk_ssize_t ztk_socket_recvfrom(ztk_socket *sock, void *buf, size_t len,
                                  char *ip, size_t ip_len, uint16_t *port)
{
    if (!sock || sock->type != ZTK_SOCK_UDP)
        return (ztk_ssize_t)ZTK_ERR_INVALID;
    return ztk_sockplat_recvfrom(sock->fd, buf, len, ip, ip_len, port);
}

int ztk_socket_sendto_batch(ztk_socket *sock, const ztk_udp_dgram_t *msgs, unsigned count)
{
    if (!sock || sock->type != ZTK_SOCK_UDP || (!msgs && count))
        return (int)ZTK_ERR_INVALID;
    return ztk_sockplat_sendmmsg(sock->fd, msgs, count);
}

int ztk_socket_recvfrom_batch(ztk_socket *sock, ztk_udp_recv_t *msgs, unsigned cap)
{
    if (!sock || sock->type != ZTK_SOCK_UDP || (!msgs && cap))
        return (int)ZTK_ERR_INVALID;
    return ztk_sockplat_recvmmsg(sock->fd, msgs, cap);
}

ztk_err_t ztk_socket_get_local(const ztk_socket *sock, char *ip, size_t ip_len, uint16_t *port)
{
    if (!sock || sock->fd < 0)
        return ZTK_ERR_INVALID;
    return ztk_sockplat_get_local(sock->fd, ip, ip_len, port);
}

ztk_err_t ztk_socket_get_peer(const ztk_socket *sock, char *ip, size_t ip_len, uint16_t *port)
{
    if (!sock || sock->fd < 0)
        return ZTK_ERR_INVALID;
    return ztk_sockplat_get_peer(sock->fd, ip, ip_len, port);
}

ztk_err_t ztk_socket_attach_poller(ztk_socket *sock, ztk_poller *poller,
                                     const ztk_socket_callbacks_t *cb, void *user)
{
    if (!sock || sock->fd < 0 || !poller)
        return ZTK_ERR_INVALID;

    ztk_socket_detach_poller(sock);

    if (cb)
        sock->cb = *cb;
    else
        memset(&sock->cb, 0, sizeof(sock->cb));
    sock->user = user;
    sock->poller = poller;
    sock->poll_events = default_poll_events(sock);

    ztk_err_t err = ztk_poller_add(poller, sock->fd, sock->poll_events, socket_poller_cb, sock);
    if (err != ZTK_OK)
        return err;
    sock->attached = 1;
    return ZTK_OK;
}

ztk_err_t ztk_socket_detach_poller(ztk_socket *sock)
{
    if (!sock || !sock->attached || !sock->poller || sock->fd < 0)
        return ZTK_OK;

    ztk_poller_del(sock->poller, sock->fd);
    sock->attached = 0;
    sock->poller = NULL;
    return ZTK_OK;
}

ztk_err_t ztk_socket_mod_events(ztk_socket *sock, unsigned events)
{
    if (!sock || !sock->attached || !sock->poller)
        return ZTK_ERR_STATE;
    sock->poll_events = events;
    return ztk_poller_mod(sock->poller, sock->fd, events);
}

static ztk_err_t udp_fd_check(const ztk_socket *sock)
{
    if (!sock || sock->type != ZTK_SOCK_UDP || sock->fd < 0)
        return ZTK_ERR_INVALID;
    return ZTK_OK;
}

ztk_err_t ztk_socket_set_broadcast(ztk_socket *sock, int on)
{
    ztk_err_t err = udp_fd_check(sock);
    if (err != ZTK_OK)
        return err;
    return ztk_sockplat_set_broadcast(sock->fd, on);
}

ztk_err_t ztk_socket_set_multicast_ttl(ztk_socket *sock, uint8_t ttl)
{
    ztk_err_t err = udp_fd_check(sock);
    if (err != ZTK_OK)
        return err;
    return ztk_sockplat_set_multicast_ttl(sock->fd, ttl);
}

ztk_err_t ztk_socket_set_multicast_if(ztk_socket *sock, const char *local_ip)
{
    ztk_err_t err = udp_fd_check(sock);
    if (err != ZTK_OK)
        return err;
    return ztk_sockplat_set_multicast_if(sock->fd, local_ip);
}

ztk_err_t ztk_socket_set_multicast_loop(ztk_socket *sock, int on)
{
    ztk_err_t err = udp_fd_check(sock);
    if (err != ZTK_OK)
        return err;
    return ztk_sockplat_set_multicast_loop(sock->fd, on);
}

ztk_err_t ztk_socket_join_multicast(ztk_socket *sock, const char *group_addr, const char *local_if)
{
    ztk_err_t err = udp_fd_check(sock);
    if (err != ZTK_OK || !group_addr)
        return err != ZTK_OK ? err : ZTK_ERR_INVALID;
    return ztk_sockplat_join_multicast(sock->fd, group_addr, local_if);
}

ztk_err_t ztk_socket_leave_multicast(ztk_socket *sock, const char *group_addr, const char *local_if)
{
    ztk_err_t err = udp_fd_check(sock);
    if (err != ZTK_OK || !group_addr)
        return err != ZTK_OK ? err : ZTK_ERR_INVALID;
    return ztk_sockplat_leave_multicast(sock->fd, group_addr, local_if);
}

ztk_err_t ztk_socket_join_multicast_filter(ztk_socket *sock, const char *group_addr,
                                             const char *src_ip, const char *local_if)
{
    ztk_err_t err = udp_fd_check(sock);
    if (err != ZTK_OK || !group_addr || !src_ip)
        return err != ZTK_OK ? err : ZTK_ERR_INVALID;
    return ztk_sockplat_join_multicast_filter(sock->fd, group_addr, src_ip, local_if);
}

ztk_err_t ztk_socket_leave_multicast_filter(ztk_socket *sock, const char *group_addr,
                                              const char *src_ip, const char *local_if)
{
    ztk_err_t err = udp_fd_check(sock);
    if (err != ZTK_OK || !group_addr || !src_ip)
        return err != ZTK_OK ? err : ZTK_ERR_INVALID;
    return ztk_sockplat_leave_multicast_filter(sock->fd, group_addr, src_ip, local_if);
}
