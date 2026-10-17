#include "ztk/net/tcp_server.h"
#include "ztk/net/socket.h"
#include "ztk/util/buf.h"
#include "ztk/poller/poller_pool.h"
#include <stdio.h>
#include <string.h>
#include "test_portable.h"

static volatile int g_sendv_ok;
static volatile int g_buf_ok;
static volatile size_t g_highwater;

static void echo_sendv_on_recv(ztk_tcp_session *session, const void *data, size_t len, void *user)
{
    (void)user;
    if (ztk_tcp_session_send(session, data, len) == ZTK_OK)
        g_sendv_ok = 1;
}

static void echo_buf_on_recv(ztk_tcp_session *session, const void *data, size_t len, void *user)
{
    ztk_buf *b;
    (void)data;
    (void)len;
    (void)user;
    b = ztk_buf_alloc(len);
    if (!b)
        return;
    memcpy((void *)ztk_buf_data(b), data, len);
    ztk_buf_set_len(b, len);
    if (ztk_tcp_session_send_buf(session, b) == ZTK_OK)
        g_buf_ok = 1;
    ztk_buf_unref(b);
}

static void on_highwater(ztk_tcp_session *session, size_t pending, void *user)
{
    (void)session;
    (void)user;
    g_highwater = pending;
}

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

static ztk_poller_pool *start_pool(unsigned size)
{
    ztk_poller_pool_opts_t popts = { .size = size, .prefer_current_thread = 0,
        .thread_priority = ZTK_THREAD_PRIO_NORMAL };
    ztk_poller_pool *pool = ztk_poller_pool_create(&popts);
    if (!pool || ztk_poller_pool_start(pool) != ZTK_OK) {
        if (pool)
            ztk_poller_pool_destroy(pool);
        return NULL;
    }
    return pool;
}

static int run_sendv_echo(void)
{
    ztk_poller_pool *pool = start_pool(1);
    ztk_tcp_session_ops_t ops = { echo_sendv_on_recv, NULL, NULL };
    ztk_tcp_server_opts_t opts = {
        .host = "127.0.0.1",
        .port = 0,
        .backlog = 64,
        .poller_pool = pool,
        .session_ops = &ops,
        .manager_interval_sec = -1.0f
    };
    ztk_tcp_server *srv;
    ztk_socket *client;
    char a[1024];
    char b[4096];
    char out[5120];
    int pending = 0;
    ztk_ssize_t rn;

    if (!pool)
        return 1;

    memset(a, 'A', sizeof(a));
    memset(b, 'B', sizeof(b));

    srv = ztk_tcp_server_create(&opts);
    if (!srv || ztk_tcp_server_start(srv) != ZTK_OK) {
        ztk_poller_pool_stop(pool);
        ztk_poller_pool_destroy(pool);
        return 1;
    }

    client = ztk_socket_create();
    if (ztk_socket_connect(client, "127.0.0.1", ztk_tcp_server_port(srv), &pending) != ZTK_OK) {
        ztk_tcp_server_destroy(srv);
        ztk_poller_pool_stop(pool);
        ztk_poller_pool_destroy(pool);
        return 1;
    }
    if (pending && wait_connect(client, 3000) != ZTK_OK) {
        ztk_tcp_server_destroy(srv);
        ztk_poller_pool_stop(pool);
        ztk_poller_pool_destroy(pool);
        return 1;
    }

    g_sendv_ok = 0;
    if (ztk_socket_sendv(client, (const ztk_socket_iov[]){
            { a, sizeof(a) }, { b, sizeof(b) }
        }, 2) != (ztk_ssize_t)(sizeof(a) + sizeof(b))) {
        ztk_socket_destroy(client);
        ztk_tcp_server_destroy(srv);
        ztk_poller_pool_stop(pool);
        ztk_poller_pool_destroy(pool);
        return 1;
    }

    for (int i = 0; i < 200 && !g_sendv_ok; ++i)
        ztk_test_sleep_ms(10);

    rn = ztk_socket_recv(client, out, sizeof(out));
    ztk_socket_destroy(client);
    ztk_tcp_server_destroy(srv);
    ztk_poller_pool_stop(pool);
    ztk_poller_pool_destroy(pool);

    if (!g_sendv_ok || rn != (ztk_ssize_t)(sizeof(a) + sizeof(b)))
        return 1;
    if (memcmp(out, a, sizeof(a)) != 0 || memcmp(out + sizeof(a), b, sizeof(b)) != 0)
        return 1;
    return 0;
}

static int run_send_buf_shared(void)
{
    ztk_poller_pool *pool = start_pool(1);
    ztk_tcp_session_ops_t ops = { echo_buf_on_recv, NULL, NULL };
    ztk_tcp_server_opts_t opts = {
        .host = "127.0.0.1",
        .port = 0,
        .backlog = 64,
        .poller_pool = pool,
        .session_ops = &ops,
        .manager_interval_sec = -1.0f,
        .out_highwater_bytes = 1,
        .on_out_highwater = on_highwater
    };
    ztk_tcp_server *srv;
    ztk_socket *clients[4];
    ztk_buf *shared;
    char payload[256];
    char buf[256];
    int i;
    int pending = 0;

    if (!pool)
        return 1;

    memset(payload, 'Z', sizeof(payload));
    shared = ztk_buf_alloc(sizeof(payload));
    if (!shared)
        goto fail;
    memcpy((void *)ztk_buf_data(shared), payload, sizeof(payload));
    ztk_buf_set_len(shared, sizeof(payload));

    srv = ztk_tcp_server_create(&opts);
    if (!srv || ztk_tcp_server_start(srv) != ZTK_OK)
        goto fail;

    for (i = 0; i < 4; ++i) {
        clients[i] = ztk_socket_create();
        if (!clients[i])
            goto fail;
        if (ztk_socket_connect(clients[i], "127.0.0.1", ztk_tcp_server_port(srv), &pending) != ZTK_OK)
            goto fail;
        if (pending && wait_connect(clients[i], 3000) != ZTK_OK)
            goto fail;
        pending = 0;
        g_buf_ok = 0;
        if (ztk_socket_send(clients[i], payload, sizeof(payload)) != (ztk_ssize_t)sizeof(payload))
            goto fail;
    }

    for (i = 0; i < 4; ++i) {
        ztk_ssize_t rn = 0;
        for (int w = 0; w < 300 && rn != (ztk_ssize_t)sizeof(payload); ++w) {
            rn = ztk_socket_recv(clients[i], buf, sizeof(buf));
            if (rn != (ztk_ssize_t)sizeof(payload))
                ztk_test_sleep_ms(10);
        }
        if (rn != (ztk_ssize_t)sizeof(payload) || memcmp(buf, payload, sizeof(payload)) != 0)
            goto fail;
        ztk_socket_destroy(clients[i]);
        clients[i] = NULL;
    }
    ztk_tcp_server_destroy(srv);
    ztk_poller_pool_stop(pool);
    ztk_poller_pool_destroy(pool);
    ztk_buf_unref(shared);
    return 0;

fail:
    for (i = 0; i < 4; ++i) {
        if (clients[i])
            ztk_socket_destroy(clients[i]);
    }
    if (srv)
        ztk_tcp_server_destroy(srv);
    if (pool) {
        ztk_poller_pool_stop(pool);
        ztk_poller_pool_destroy(pool);
    }
    ztk_buf_unref(shared);
    return 1;
}

int main(void)
{
    if (run_sendv_echo() != 0) {
        fprintf(stderr, "sendv echo failed\n");
        return 1;
    }
    if (run_send_buf_shared() != 0) {
        fprintf(stderr, "send_buf shared failed\n");
        return 1;
    }
    printf("test_tcp_sendv ok\n");
    return 0;
}
