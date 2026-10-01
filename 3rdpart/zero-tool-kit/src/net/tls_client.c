#include "ztk/net/tls_client.h"
#include "ztk/net/ssl.h"
#include "ztk/net/socket.h"
#include "ztk/poller/poller.h"
#include "internal/ssl_ctx_internal.h"
#if defined(_WIN32)
#  include <winsock2.h>
#endif
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TLS_RECV_BUF 16384
#define TLS_SEND_BUF 65536

typedef enum {
    TLS_ST_TCP = 0,
    TLS_ST_HANDSHAKE,
    TLS_ST_READY,
} tls_state_t;

struct ztk_tls_client {
    ztk_tls_client_opts_t opts;
    ztk_socket *sock;
    SSL *ssl_handle;
    tls_state_t state;
    int tcp_connected;
    int closing;
    int ready_sent;
    char host_for_connect[256];
    char sni_name[256];
    uint8_t pending[TLS_SEND_BUF];
    size_t pending_len;
};

static void tls_fire_error(ztk_tls_client *c);
static void tls_mod_poll(ztk_tls_client *c, int want_write);
static ztk_err_t tls_do_handshake(ztk_tls_client *c);
static void tls_drain_plain(ztk_tls_client *c);
static void tls_flush_pending(ztk_tls_client *c);

static void tls_mod_poll(ztk_tls_client *c, int want_write)
{
    if (!c || !c->sock)
        return;
    unsigned ev = ZTK_POLL_IN | ZTK_POLL_ERR | ZTK_POLL_HUP;
    if (want_write || !c->tcp_connected)
        ev |= ZTK_POLL_OUT;
    ztk_socket_mod_events(c->sock, ev);
}

static void tls_fire_error(ztk_tls_client *c)
{
    if (!c || c->closing)
        return;
    c->closing = 1;
    ztk_socket_detach_poller(c->sock);
    if (c->ssl_handle) {
        SSL_free(c->ssl_handle);
        c->ssl_handle = NULL;
    }
    c->state = TLS_ST_TCP;
    c->tcp_connected = 0;
    c->ready_sent = 0;
    c->pending_len = 0;
    if (c->opts.ops && c->opts.ops->on_error)
        c->opts.ops->on_error(c, c->opts.user);
}

static ztk_err_t tls_ssl_error(ztk_tls_client *c, int ret, const char *where)
{
    int err = SSL_get_error(c->ssl_handle, ret);
    if (err == SSL_ERROR_WANT_READ) {
        tls_mod_poll(c, 0);
        return ZTK_ERR_AGAIN;
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        tls_mod_poll(c, 1);
        return ZTK_ERR_AGAIN;
    }
    char buf[256];
    unsigned long e = ERR_get_error();
    if (e)
        ERR_error_string_n(e, buf, sizeof(buf));
    else
        snprintf(buf, sizeof(buf), "%s ssl_err=%d", where ? where : "ssl", err);
    ztk_set_last_error(buf);
    tls_fire_error(c);
    return ZTK_ERR_IO;
}

static ztk_err_t tls_do_handshake(ztk_tls_client *c)
{
    if (!c || !c->ssl_handle)
        return ZTK_ERR_INVALID;
    int r = SSL_connect(c->ssl_handle);
    if (r == 1) {
        c->state = TLS_ST_READY;
        tls_mod_poll(c, 0);
        if (!c->ready_sent) {
            c->ready_sent = 1;
            if (c->opts.ops && c->opts.ops->on_connect)
                c->opts.ops->on_connect(c, c->opts.user);
        }
        tls_flush_pending(c);
        tls_drain_plain(c);
        return ZTK_OK;
    }
    return tls_ssl_error(c, r, "SSL_connect");
}

static void tls_drain_plain(ztk_tls_client *c)
{
    if (!c || c->state != TLS_ST_READY || !c->ssl_handle || c->closing)
        return;

    uint8_t buf[TLS_RECV_BUF];
    for (;;) {
        int n = SSL_read(c->ssl_handle, buf, sizeof(buf));
        if (n > 0) {
            if (c->opts.ops && c->opts.ops->on_recv)
                c->opts.ops->on_recv(c, buf, (size_t)n, c->opts.user);
            continue;
        }
        if (n == 0) {
            tls_fire_error(c);
            return;
        }
        int err = SSL_get_error(c->ssl_handle, n);
        if (err == SSL_ERROR_WANT_READ) {
            tls_mod_poll(c, 0);
            return;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            tls_mod_poll(c, 1);
            return;
        }
        tls_ssl_error(c, n, "SSL_read");
        return;
    }
}

static void tls_flush_pending(ztk_tls_client *c)
{
    if (!c || !c->pending_len || c->state != TLS_ST_READY || !c->ssl_handle)
        return;
    size_t off = 0;
    while (off < c->pending_len) {
        int n = SSL_write(c->ssl_handle, c->pending + off, (int)(c->pending_len - off));
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (tls_ssl_error(c, n, "SSL_write") == ZTK_ERR_AGAIN) {
            if (off > 0) {
                memmove(c->pending, c->pending + off, c->pending_len - off);
                c->pending_len -= off;
            }
            return;
        }
        return;
    }
    c->pending_len = 0;
}

static void tls_start_ssl(ztk_tls_client *c)
{
    if (!c || !c->opts.ssl_ctx || !c->opts.ssl_ctx->ctx)
        return;

    c->ssl_handle = SSL_new(c->opts.ssl_ctx->ctx);
    if (!c->ssl_handle) {
        ztk_set_last_error("SSL_new failed");
        tls_fire_error(c);
        return;
    }

    if (SSL_set_fd(c->ssl_handle, (int)ztk_socket_fd(c->sock)) != 1) {
        ztk_set_last_error("SSL_set_fd failed");
        tls_fire_error(c);
        return;
    }

    const char *sni = c->sni_name[0] ? c->sni_name : c->host_for_connect;
    if (sni[0])
        (void)SSL_ctrl(c->ssl_handle, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, (void *)sni);

    c->state = TLS_ST_HANDSHAKE;
    c->ready_sent = 0;
    tls_mod_poll(c, 1);
    (void)tls_do_handshake(c);
}

static void sock_on_writable(ztk_socket *sock, void *user)
{
    ztk_tls_client *c = (ztk_tls_client *)user;
    if (!c || c->closing)
        return;

    if (!c->tcp_connected) {
        ztk_err_t err = ztk_socket_check_connect(sock);
        if (err == ZTK_OK) {
            c->tcp_connected = 1;
            tls_start_ssl(c);
            return;
        }
        if (err != ZTK_ERR_AGAIN)
            tls_fire_error(c);
        return;
    }

    if (c->state == TLS_ST_HANDSHAKE)
        (void)tls_do_handshake(c);
    else if (c->state == TLS_ST_READY) {
        tls_flush_pending(c);
        tls_drain_plain(c);
    }
}

static void sock_on_readable(ztk_socket *sock, void *user)
{
    ztk_tls_client *c = (ztk_tls_client *)user;
    if (!c || c->closing)
        return;

    if (!c->tcp_connected) {
        sock_on_writable(sock, user);
        if (!c->tcp_connected || c->closing)
            return;
    }

    if (c->state == TLS_ST_HANDSHAKE)
        (void)tls_do_handshake(c);
    else if (c->state == TLS_ST_READY)
        tls_drain_plain(c);
}

static void sock_on_error(ztk_socket *sock, void *user)
{
    (void)sock;
    tls_fire_error((ztk_tls_client *)user);
}

ztk_tls_client *ztk_tls_client_create(const ztk_tls_client_opts_t *opts)
{
    if (!opts || !opts->poller || !opts->ops || !opts->ssl_ctx || !opts->ssl_ctx->ctx)
        return NULL;
    if (ztk_ssl_global_init() != ZTK_OK)
        return NULL;

    ztk_tls_client *c = (ztk_tls_client *)calloc(1, sizeof(*c));
    if (!c)
        return NULL;

    c->opts = *opts;
    if (opts->sni_host && opts->sni_host[0])
        strncpy(c->sni_name, opts->sni_host, sizeof(c->sni_name) - 1);

    c->sock = ztk_socket_create();
    if (!c->sock) {
        free(c);
        return NULL;
    }
    return c;
}

void ztk_tls_client_destroy(ztk_tls_client *client)
{
    if (!client)
        return;
    ztk_tls_client_close(client);
    if (client->ssl_handle) {
        SSL_free(client->ssl_handle);
        client->ssl_handle = NULL;
    }
    if (client->sock) {
        ztk_socket_destroy(client->sock);
        client->sock = NULL;
    }
    free(client);
}

ztk_err_t ztk_tls_client_connect(ztk_tls_client *client, const char *host, uint16_t port)
{
    if (!client || client->closing || !host)
        return ZTK_ERR_INVALID;

    ztk_socket_detach_poller(client->sock);
    if (client->ssl_handle) {
        SSL_free(client->ssl_handle);
        client->ssl_handle = NULL;
    }
    client->state = TLS_ST_TCP;
    client->tcp_connected = 0;
    client->ready_sent = 0;
    client->pending_len = 0;

    strncpy(client->host_for_connect, host, sizeof(client->host_for_connect) - 1);
    if (!client->sni_name[0])
        strncpy(client->sni_name, host, sizeof(client->sni_name) - 1);

    int pending = 0;
    ztk_err_t err = ztk_socket_connect(client->sock, host, port, &pending);
    if (err != ZTK_OK)
        return err;

    ztk_socket_callbacks_t cb = { sock_on_readable, sock_on_writable, sock_on_error };
    err = ztk_socket_attach_poller(client->sock, client->opts.poller, &cb, client);
    if (err != ZTK_OK)
        return err;

    if (!pending) {
        client->tcp_connected = 1;
        tls_start_ssl(client);
    }
    return ZTK_OK;
}

int ztk_tls_client_is_connected(const ztk_tls_client *client)
{
    return client && client->state == TLS_ST_READY && !client->closing && client->ready_sent;
}

ztk_err_t ztk_tls_client_send(ztk_tls_client *client, const void *data, size_t len)
{
    if (!client || client->closing || !data || len == 0)
        return ZTK_ERR_INVALID;

    if (client->state != TLS_ST_READY || !client->ssl_handle) {
        if (client->pending_len + len > TLS_SEND_BUF)
            return ZTK_ERR_BUFFER_TOO_SMALL;
        memcpy(client->pending + client->pending_len, data, len);
        client->pending_len += len;
        return ZTK_ERR_AGAIN;
    }

    const uint8_t *p = (const uint8_t *)data;
    size_t off = 0;
    while (off < len) {
        int n = SSL_write(client->ssl_handle, p + off, (int)(len - off));
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        ztk_err_t err = tls_ssl_error(client, n, "SSL_write");
        if (err == ZTK_ERR_AGAIN && off < len) {
            size_t rest = len - off;
            if (client->pending_len + rest > TLS_SEND_BUF)
                return ZTK_ERR_BUFFER_TOO_SMALL;
            memcpy(client->pending + client->pending_len, p + off, rest);
            client->pending_len += rest;
        }
        return err;
    }
    return ZTK_OK;
}

void ztk_tls_client_close(ztk_tls_client *client)
{
    if (!client || client->closing)
        return;
    client->closing = 1;
    ztk_socket_detach_poller(client->sock);
    if (client->ssl_handle) {
        SSL_shutdown(client->ssl_handle);
        SSL_free(client->ssl_handle);
        client->ssl_handle = NULL;
    }
    ztk_socket_destroy(client->sock);
    client->sock = ztk_socket_create();
    client->state = TLS_ST_TCP;
    client->tcp_connected = 0;
    client->ready_sent = 0;
    client->pending_len = 0;
    client->closing = 0;
}
