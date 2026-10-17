#include "net/socket.h"
#include "poller/poller.h"
#include "internal/sock_platform.h"
#include <stdlib.h>
#include <string.h>

struct ntk_socket {
    int fd;
    ntk_sock_type_t type;
    ntk_poller *poller;
    ntk_socket_callbacks_t cb;
    void *user;
    unsigned poll_events;
    int attached;
    int connect_pending;
};

static int s_sockplat_inited;

static ntk_err_t ensure_sockplat(void)
{
    if (s_sockplat_inited)
        return NTK_OK;
    ntk_err_t err = ntk_sockplat_init();
    if (err == NTK_OK)
        s_sockplat_inited = 1;
    return err;
}

static void close_fd(ntk_socket *sock)
{
    if (!sock || sock->fd < 0)
        return;
    ntk_sockplat_close(sock->fd);
    sock->fd = -1;
}

static unsigned default_poll_events(const ntk_socket *sock)
{
    unsigned ev = NTK_POLL_IN | NTK_POLL_ERR | NTK_POLL_HUP;
    if (sock->connect_pending)
        ev |= NTK_POLL_OUT;
    return ev;
}

static void socket_poller_cb(int fd, unsigned events, void *user)
{
    ntk_socket *sock = (ntk_socket *)user;
    if (!sock || sock->fd != fd)
        return;

    if (events & (NTK_POLL_ERR | NTK_POLL_HUP)) {
        if (sock->cb.on_error)
            sock->cb.on_error(sock, sock->user);
        return;
    }

    if ((events & NTK_POLL_OUT) && sock->connect_pending) {
        if (ntk_socket_check_connect(sock) == NTK_OK) {
            sock->connect_pending = 0;
            sock->type = NTK_SOCK_TCP_CONNECTED;
            if (sock->attached)
                ntk_socket_mod_events(sock, default_poll_events(sock));
        } else if (sock->cb.on_error) {
            sock->cb.on_error(sock, sock->user);
            return;
        }
    }

    if (events & NTK_POLL_OUT) {
        if (sock->cb.on_writable)
            sock->cb.on_writable(sock, sock->user);
    }

    if (events & NTK_POLL_IN) {
        if (sock->cb.on_readable)
            sock->cb.on_readable(sock, sock->user);
    }
}

ntk_socket *ntk_socket_create(void)
{
    ntk_socket *sock = (ntk_socket *)calloc(1, sizeof(*sock));
    if (sock)
        sock->fd = -1;
    return sock;
}

void ntk_socket_destroy(ntk_socket *sock)
{
    if (!sock)
        return;
    ntk_socket_detach_poller(sock);
    close_fd(sock);
    free(sock);
}

ntk_err_t ntk_socket_adopt(ntk_socket *sock, int fd, ntk_sock_type_t type)
{
    if (!sock || fd < 0)
        return NTK_ERR_INVALID;
    if (ensure_sockplat() != NTK_OK)
        return NTK_ERR_PLATFORM;

    ntk_socket_detach_poller(sock);
    close_fd(sock);

    ntk_err_t err = NTK_OK;
    switch (type) {
    case NTK_SOCK_TCP_LISTEN:
        err = ntk_sockplat_apply_tcp_server_opts(fd);
        break;
    case NTK_SOCK_TCP_CLIENT:
    case NTK_SOCK_TCP_CONNECTED:
        err = ntk_sockplat_apply_tcp_client_opts(fd);
        break;
    case NTK_SOCK_UDP:
        err = ntk_sockplat_apply_udp_opts(fd);
        break;
    default:
        return NTK_ERR_INVALID;
    }
    if (err != NTK_OK)
        return err;

    sock->fd = fd;
    sock->type = type;
    sock->connect_pending = 0;
    return NTK_OK;
}

int ntk_socket_fd(const ntk_socket *sock)
{
    return sock ? sock->fd : -1;
}

ntk_sock_type_t ntk_socket_type(const ntk_socket *sock)
{
    return sock ? sock->type : NTK_SOCK_NONE;
}

ntk_err_t ntk_socket_listen(ntk_socket *sock, const char *host, uint16_t port, int backlog)
{
    if (!sock)
        return NTK_ERR_INVALID;
    if (ensure_sockplat() != NTK_OK)
        return NTK_ERR_PLATFORM;

    ntk_socket_detach_poller(sock);
    close_fd(sock);

    int fd = -1;
    ntk_err_t err = ntk_sockplat_listen(&fd, host, port, backlog);
    if (err != NTK_OK)
        return err;

    sock->fd = fd;
    sock->type = NTK_SOCK_TCP_LISTEN;
    sock->connect_pending = 0;
    return NTK_OK;
}

ntk_err_t ntk_socket_bind_udp_ex(ntk_socket *sock, const char *host, uint16_t port, int reuse_addr,
                                   int reuse_port)
{
    if (!sock)
        return NTK_ERR_INVALID;
    if (ensure_sockplat() != NTK_OK)
        return NTK_ERR_PLATFORM;

    ntk_socket_detach_poller(sock);
    close_fd(sock);

    int fd = -1;
    ntk_err_t err = ntk_sockplat_bind_udp(&fd, host, port, reuse_addr, reuse_port);
    if (err != NTK_OK)
        return err;

    sock->fd = fd;
    sock->type = NTK_SOCK_UDP;
    sock->connect_pending = 0;
    return NTK_OK;
}

ntk_err_t ntk_socket_bind_udp(ntk_socket *sock, const char *host, uint16_t port, int reuse)
{
    return ntk_socket_bind_udp_ex(sock, host, port, reuse, 0);
}

ntk_err_t ntk_socket_connect(ntk_socket *sock, const char *host, uint16_t port, int *in_progress)
{
    if (!sock || !host)
        return NTK_ERR_INVALID;
    if (ensure_sockplat() != NTK_OK)
        return NTK_ERR_PLATFORM;

    ntk_socket_detach_poller(sock);
    close_fd(sock);

    int fd = -1;
    int pending = 0;
    ntk_err_t err = ntk_sockplat_connect(&fd, host, port, &pending);
    if (err != NTK_OK)
        return err;

    sock->fd = fd;
    sock->type = pending ? NTK_SOCK_TCP_CLIENT : NTK_SOCK_TCP_CONNECTED;
    sock->connect_pending = pending ? 1 : 0;
    if (in_progress)
        *in_progress = pending;
    return NTK_OK;
}

ntk_err_t ntk_socket_check_connect(ntk_socket *sock)
{
    if (!sock || sock->fd < 0)
        return NTK_ERR_INVALID;
    if (!sock->connect_pending)
        return NTK_OK;
    ntk_err_t err = ntk_sockplat_check_connect(sock->fd);
    if (err == NTK_OK) {
        sock->connect_pending = 0;
        sock->type = NTK_SOCK_TCP_CONNECTED;
    }
    return err;
}

ntk_err_t ntk_socket_accept(ntk_socket *listen_sock, ntk_socket **out_client)
{
    if (!listen_sock || listen_sock->type != NTK_SOCK_TCP_LISTEN || !out_client)
        return NTK_ERR_INVALID;

    int cfd = -1;
    ntk_err_t err = ntk_sockplat_accept(listen_sock->fd, &cfd);
    if (err == NTK_ERR_AGAIN)
        return NTK_ERR_AGAIN;
    if (err != NTK_OK)
        return err;

    ntk_socket *client = ntk_socket_create();
    if (!client) {
        ntk_sockplat_close(cfd);
        return NTK_ERR_NOMEM;
    }
    client->fd = cfd;
    client->type = NTK_SOCK_TCP_CONNECTED;
    client->connect_pending = 0;
    *out_client = client;
    return NTK_OK;
}

ntk_ssize_t ntk_socket_send(ntk_socket *sock, const void *data, size_t len)
{
    if (!sock || sock->fd < 0)
        return (ntk_ssize_t)NTK_ERR_INVALID;
    return ntk_sockplat_send(sock->fd, data, len);
}

ntk_ssize_t ntk_socket_sendv(ntk_socket *sock, const ntk_socket_iov *iov, unsigned count)
{
    ntk_sock_iov plat[NTK_SOCK_IOV_MAX];
    unsigned i;

    if (!sock || sock->fd < 0)
        return (ntk_ssize_t)NTK_ERR_INVALID;
    if (!iov || count == 0)
        return (ntk_ssize_t)NTK_ERR_INVALID;
    if (count > NTK_SOCKET_IOV_MAX)
        count = NTK_SOCKET_IOV_MAX;

    for (i = 0; i < count; ++i) {
        plat[i].base = iov[i].base;
        plat[i].len = iov[i].len;
    }
    return ntk_sockplat_sendv(sock->fd, plat, count);
}

ntk_err_t ntk_socket_set_bbr(ntk_socket *sock, int on)
{
    if (!sock || sock->fd < 0)
        return NTK_ERR_INVALID;
    if (sock->type != NTK_SOCK_TCP_CONNECTED && sock->type != NTK_SOCK_TCP_CLIENT)
        return NTK_ERR_INVALID;
    return ntk_sockplat_set_tcp_bbr(sock->fd, on);
}

ntk_ssize_t ntk_socket_recv(ntk_socket *sock, void *buf, size_t len)
{
    if (!sock || sock->fd < 0)
        return (ntk_ssize_t)NTK_ERR_INVALID;
    return ntk_sockplat_recv(sock->fd, buf, len);
}

ntk_ssize_t ntk_socket_sendto(ntk_socket *sock, const void *data, size_t len, const char *ip, uint16_t port)
{
    if (!sock || sock->type != NTK_SOCK_UDP)
        return (ntk_ssize_t)NTK_ERR_INVALID;
    return ntk_sockplat_sendto(sock->fd, data, len, ip, port);
}

ntk_ssize_t ntk_socket_recvfrom(ntk_socket *sock, void *buf, size_t len,
                                  char *ip, size_t ip_len, uint16_t *port)
{
    if (!sock || sock->type != NTK_SOCK_UDP)
        return (ntk_ssize_t)NTK_ERR_INVALID;
    return ntk_sockplat_recvfrom(sock->fd, buf, len, ip, ip_len, port);
}

int ntk_socket_sendto_batch(ntk_socket *sock, const ntk_udp_dgram_t *msgs, unsigned count)
{
    if (!sock || sock->type != NTK_SOCK_UDP || (!msgs && count))
        return (int)NTK_ERR_INVALID;
    return ntk_sockplat_sendmmsg(sock->fd, msgs, count);
}

int ntk_socket_recvfrom_batch(ntk_socket *sock, ntk_udp_recv_t *msgs, unsigned cap)
{
    if (!sock || sock->type != NTK_SOCK_UDP || (!msgs && cap))
        return (int)NTK_ERR_INVALID;
    return ntk_sockplat_recvmmsg(sock->fd, msgs, cap);
}

ntk_err_t ntk_socket_get_local(const ntk_socket *sock, char *ip, size_t ip_len, uint16_t *port)
{
    if (!sock || sock->fd < 0)
        return NTK_ERR_INVALID;
    return ntk_sockplat_get_local(sock->fd, ip, ip_len, port);
}

ntk_err_t ntk_socket_get_peer(const ntk_socket *sock, char *ip, size_t ip_len, uint16_t *port)
{
    if (!sock || sock->fd < 0)
        return NTK_ERR_INVALID;
    return ntk_sockplat_get_peer(sock->fd, ip, ip_len, port);
}

ntk_err_t ntk_socket_attach_poller(ntk_socket *sock, ntk_poller *poller,
                                     const ntk_socket_callbacks_t *cb, void *user)
{
    if (!sock || sock->fd < 0 || !poller)
        return NTK_ERR_INVALID;

    ntk_socket_detach_poller(sock);

    if (cb)
        sock->cb = *cb;
    else
        memset(&sock->cb, 0, sizeof(sock->cb));
    sock->user = user;
    sock->poller = poller;
    sock->poll_events = default_poll_events(sock);

    ntk_err_t err = ntk_poller_add(poller, sock->fd, sock->poll_events, socket_poller_cb, sock);
    if (err != NTK_OK)
        return err;
    sock->attached = 1;
    return NTK_OK;
}

ntk_err_t ntk_socket_detach_poller(ntk_socket *sock)
{
    if (!sock || !sock->attached || !sock->poller || sock->fd < 0)
        return NTK_OK;

    ntk_poller_del(sock->poller, sock->fd);
    sock->attached = 0;
    sock->poller = NULL;
    return NTK_OK;
}

ntk_err_t ntk_socket_mod_events(ntk_socket *sock, unsigned events)
{
    if (!sock || !sock->attached || !sock->poller)
        return NTK_ERR_STATE;
    sock->poll_events = events;
    return ntk_poller_mod(sock->poller, sock->fd, events);
}

static ntk_err_t udp_fd_check(const ntk_socket *sock)
{
    if (!sock || sock->type != NTK_SOCK_UDP || sock->fd < 0)
        return NTK_ERR_INVALID;
    return NTK_OK;
}

ntk_err_t ntk_socket_set_broadcast(ntk_socket *sock, int on)
{
    ntk_err_t err = udp_fd_check(sock);
    if (err != NTK_OK)
        return err;
    return ntk_sockplat_set_broadcast(sock->fd, on);
}

ntk_err_t ntk_socket_set_multicast_ttl(ntk_socket *sock, uint8_t ttl)
{
    ntk_err_t err = udp_fd_check(sock);
    if (err != NTK_OK)
        return err;
    return ntk_sockplat_set_multicast_ttl(sock->fd, ttl);
}

ntk_err_t ntk_socket_set_multicast_if(ntk_socket *sock, const char *local_ip)
{
    ntk_err_t err = udp_fd_check(sock);
    if (err != NTK_OK)
        return err;
    return ntk_sockplat_set_multicast_if(sock->fd, local_ip);
}

ntk_err_t ntk_socket_set_multicast_loop(ntk_socket *sock, int on)
{
    ntk_err_t err = udp_fd_check(sock);
    if (err != NTK_OK)
        return err;
    return ntk_sockplat_set_multicast_loop(sock->fd, on);
}

ntk_err_t ntk_socket_join_multicast(ntk_socket *sock, const char *group_addr, const char *local_if)
{
    ntk_err_t err = udp_fd_check(sock);
    if (err != NTK_OK || !group_addr)
        return err != NTK_OK ? err : NTK_ERR_INVALID;
    return ntk_sockplat_join_multicast(sock->fd, group_addr, local_if);
}

ntk_err_t ntk_socket_leave_multicast(ntk_socket *sock, const char *group_addr, const char *local_if)
{
    ntk_err_t err = udp_fd_check(sock);
    if (err != NTK_OK || !group_addr)
        return err != NTK_OK ? err : NTK_ERR_INVALID;
    return ntk_sockplat_leave_multicast(sock->fd, group_addr, local_if);
}

ntk_err_t ntk_socket_join_multicast_filter(ntk_socket *sock, const char *group_addr,
                                             const char *src_ip, const char *local_if)
{
    ntk_err_t err = udp_fd_check(sock);
    if (err != NTK_OK || !group_addr || !src_ip)
        return err != NTK_OK ? err : NTK_ERR_INVALID;
    return ntk_sockplat_join_multicast_filter(sock->fd, group_addr, src_ip, local_if);
}

ntk_err_t ntk_socket_leave_multicast_filter(ntk_socket *sock, const char *group_addr,
                                              const char *src_ip, const char *local_if)
{
    ntk_err_t err = udp_fd_check(sock);
    if (err != NTK_OK || !group_addr || !src_ip)
        return err != NTK_OK ? err : NTK_ERR_INVALID;
    return ntk_sockplat_leave_multicast_filter(sock->fd, group_addr, src_ip, local_if);
}
