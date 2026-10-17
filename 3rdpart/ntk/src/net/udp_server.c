#include "net/udp_server.h"
#include "net/socket.h"
#include "poller/poller.h"
#include "poller/poller_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NTK_UDP_RECV 65536

typedef struct ntk_udp_slot {
    ntk_udp_server *server;
    ntk_socket *sock;
    ntk_poller *poller;
} ntk_udp_slot;

struct ntk_udp_server {
    ntk_udp_slot *slots;
    unsigned slot_count;

    ntk_udp_server_ops_t ops;
    void *user;

    char host[64];
    uint16_t port;
    int reuse_addr;
    int reuse_port;
    /** 无 SO_REUSEPORT 时多 poller 共享同一 fd（非内核级抢占） */
    int shared_fd_mode;
    int started;
};

static int sockplat_has_reuseport(void)
{
#if defined(SO_REUSEPORT)
    return 1;
#else
    return 0;
#endif
}

static void udp_on_readable(int fd, unsigned events, void *user)
{
    ntk_udp_slot *slot = (ntk_udp_slot *)user;
    ntk_udp_server *srv;
    (void)events;

    if (!slot || !slot->sock || !slot->server)
        return;
    if (ntk_socket_fd(slot->sock) != fd)
        return;

    srv = slot->server;
    if (!srv->ops.on_packet)
        return;

    ntk_socket *sock = srv->shared_fd_mode ? srv->slots[0].sock : slot->sock;

    char buf[NTK_UDP_RECV];
    char peer_ip[64];
    uint16_t peer_port = 0;

    for (;;) {
        ntk_ssize_t n = ntk_socket_recvfrom(sock, buf, sizeof(buf), peer_ip, sizeof(peer_ip), &peer_port);
        if (n > 0) {
            srv->ops.on_packet(srv, peer_ip, peer_port, buf, (size_t)n, srv->user);
            continue;
        }
        if (n == NTK_ERR_AGAIN)
            break;
        break;
    }
}

static ntk_poller *resolve_poller(const ntk_udp_server_opts_t *opts, unsigned index)
{
    if (opts->poller_pool)
        return ntk_poller_pool_at(opts->poller_pool, index);
    if (opts->poller && index == 0)
        return opts->poller;
    return NULL;
}

static unsigned resolve_poller_count(const ntk_udp_server_opts_t *opts)
{
    if (opts->poller_pool)
        return ntk_poller_pool_size(opts->poller_pool);
    return opts->poller ? 1 : 0;
}

ntk_udp_server *ntk_udp_server_create(const ntk_udp_server_opts_t *opts)
{
    if (!opts || !opts->ops)
        return NULL;

    unsigned n = resolve_poller_count(opts);
    if (n == 0)
        return NULL;

    ntk_udp_server *srv = (ntk_udp_server *)calloc(1, sizeof(*srv));
    if (!srv)
        return NULL;

    srv->slot_count = n;
    srv->slots = (ntk_udp_slot *)calloc(n, sizeof(*srv->slots));
    if (!srv->slots) {
        free(srv);
        return NULL;
    }

    for (unsigned i = 0; i < n; ++i) {
        srv->slots[i].server = srv;
        srv->slots[i].poller = resolve_poller(opts, i);
        srv->slots[i].sock = ntk_socket_create();
        if (!srv->slots[i].poller || !srv->slots[i].sock) {
            ntk_udp_server_destroy(srv);
            return NULL;
        }
    }

    srv->ops = *opts->ops;
    srv->user = opts->user;
    srv->reuse_addr = opts->reuse ? 1 : 0;
    srv->reuse_port = opts->reuse_port ? 1 : 0;
    if (n > 1) {
        srv->reuse_addr = 1;
        srv->reuse_port = 1;
    }
    if (opts->host)
        snprintf(srv->host, sizeof(srv->host), "%s", opts->host);
    srv->port = opts->port;
    return srv;
}

void ntk_udp_server_destroy(ntk_udp_server *srv)
{
    if (!srv)
        return;
    ntk_udp_server_stop(srv);
    if (srv->slots) {
        for (unsigned i = 0; i < srv->slot_count; ++i) {
            if (srv->slots[i].sock)
                ntk_socket_destroy(srv->slots[i].sock);
        }
        free(srv->slots);
    }
    free(srv);
}

static ntk_err_t udp_server_register_slot(ntk_udp_server *srv, ntk_udp_slot *slot)
{
    int fd = ntk_socket_fd(slot->sock);
    return ntk_poller_add(slot->poller, fd, NTK_POLL_IN | NTK_POLL_ERR | NTK_POLL_HUP, udp_on_readable,
                           slot);
}

ntk_err_t ntk_udp_server_start(ntk_udp_server *srv)
{
    if (!srv || srv->started)
        return srv && srv->started ? NTK_OK : NTK_ERR_INVALID;

    const char *host = srv->host[0] ? srv->host : NULL;
    srv->shared_fd_mode = 0;

    if (srv->slot_count > 1 && srv->reuse_port && !sockplat_has_reuseport())
        srv->shared_fd_mode = 1;

    if (srv->shared_fd_mode) {
        ntk_udp_slot *slot0 = &srv->slots[0];
        ntk_err_t err =
            ntk_socket_bind_udp_ex(slot0->sock, host, srv->port, srv->reuse_addr, 0);
        if (err != NTK_OK)
            return err;
        if (ntk_socket_get_local(slot0->sock, NULL, 0, &srv->port) != NTK_OK)
            srv->port = 0;

        {
            int fd = ntk_socket_fd(slot0->sock);
            for (unsigned i = 0; i < srv->slot_count; ++i) {
                err = ntk_poller_add(srv->slots[i].poller, fd, NTK_POLL_IN | NTK_POLL_ERR | NTK_POLL_HUP,
                                      udp_on_readable, &srv->slots[i]);
                if (err != NTK_OK) {
                    ntk_udp_server_stop(srv);
                    return err;
                }
            }
        }
        srv->started = 1;
        return NTK_OK;
    }

    for (unsigned i = 0; i < srv->slot_count; ++i) {
        ntk_udp_slot *slot = &srv->slots[i];
        ntk_err_t err =
            ntk_socket_bind_udp_ex(slot->sock, host, srv->port, srv->reuse_addr, srv->reuse_port);
        if (err != NTK_OK) {
            ntk_udp_server_stop(srv);
            return err;
        }

        if (i == 0) {
            if (ntk_socket_get_local(slot->sock, NULL, 0, &srv->port) != NTK_OK)
                srv->port = 0;
        }

        err = udp_server_register_slot(srv, slot);
        if (err != NTK_OK) {
            ntk_udp_server_stop(srv);
            return err;
        }
    }

    srv->started = 1;
    return NTK_OK;
}

void ntk_udp_server_stop(ntk_udp_server *srv)
{
    if (!srv || !srv->started)
        return;

    if (srv->shared_fd_mode) {
        int fd = ntk_socket_fd(srv->slots[0].sock);
        for (unsigned i = 0; i < srv->slot_count; ++i) {
            if (srv->slots[i].poller)
                ntk_poller_del(srv->slots[i].poller, fd);
        }
    } else {
        for (unsigned i = 0; i < srv->slot_count; ++i) {
            ntk_udp_slot *slot = &srv->slots[i];
            if (slot->sock && slot->poller)
                ntk_poller_del(slot->poller, ntk_socket_fd(slot->sock));
        }
    }

    srv->started = 0;
    srv->shared_fd_mode = 0;
}

uint16_t ntk_udp_server_port(const ntk_udp_server *srv)
{
    return srv ? srv->port : 0;
}

unsigned ntk_udp_server_poller_count(const ntk_udp_server *srv)
{
    return srv ? srv->slot_count : 0;
}

ntk_poller *ntk_udp_server_poller(const ntk_udp_server *srv, unsigned index)
{
    if (!srv || index >= srv->slot_count)
        return NULL;
    return srv->slots[index].poller;
}

ntk_err_t ntk_udp_server_sendto(ntk_udp_server *srv, const char *ip, uint16_t port, const void *data,
                                  size_t len)
{
    if (!srv || !srv->started || !ip || srv->slot_count == 0)
        return NTK_ERR_STATE;
    /* 任一同端口 socket 均可发送 */
    ntk_ssize_t n = ntk_socket_sendto(srv->slots[0].sock, data, len, ip, port);
    if (n < 0)
        return (ntk_err_t)n;
    return NTK_OK;
}
