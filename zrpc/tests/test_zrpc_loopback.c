#include <zrpc/zrpc.h>
#include <ztk/platform.h>
#include <ztk/thread/sem.h>

#include <stdio.h>
#include <string.h>

/* 服务端 handler：原样回显。 */
static void echo_handler(zrpc_call_t *call, const zrpc_request_t *req, void *user) {
    (void)user;
    zrpc_reply_bytes(call, 0, req->payload.data, req->payload.len);
}

typedef struct client_state {
    ztk_sem *sem;
    int status;
    char buf[8192];
    size_t len;
} client_state_t;

static void on_response(int status, const zrpc_response_t *resp, void *user) {
    client_state_t *st = (client_state_t *)user;
    st->status = status;
    if (status == ZRPC_OK && resp && resp->payload.len <= sizeof(st->buf)) {
        memcpy(st->buf, resp->payload.data, resp->payload.len);
        st->len = resp->payload.len;
        st->buf[resp->payload.len < sizeof(st->buf) ? resp->payload.len : sizeof(st->buf) - 1] = '\0';
    }
    ztk_sem_post(st->sem, 1);
}

static int wait_response(client_state_t *st, unsigned timeout_ms) {
    if (ztk_sem_timedwait(st->sem, timeout_ms) != ZTK_OK) {
        return -1;
    }
    return 0;
}

/* 反复尝试直到发现收敛或超时。返回最终 status。 */
static int run_call(zrpc_proxy_t *proxy, client_state_t *st, const void *data, size_t len) {
    int attempt;
    zrpc_payload_t pl;
    pl.data = data;
    pl.len = len;
    pl.encoding = 0;
    for (attempt = 0; attempt < 40; attempt++) {
        int rc;
        st->status = -999;
        st->len = 0;
        rc = zrpc_proxy_call(proxy, "echo", "ping", &pl, on_response, st, 1000, NULL);
        if (rc != ZRPC_OK) {
            return rc;
        }
        if (wait_response(st, 1000) == 0) {
            if (st->status == ZRPC_ERR_NOTFOUND) {
                ztk_sleep_ms(50);
                continue;
            }
            return st->status;
        }
        ztk_sleep_ms(50);
    }
    return -1;
}

static int run_case(zrpc_transport_kind_t kind, const char *label) {
    zrpc_node_config_t scfg;
    zrpc_node_config_t ccfg;
    zrpc_node_t *server = NULL;
    zrpc_node_t *client = NULL;
    zrpc_service_t *svc = NULL;
    zrpc_proxy_t *proxy = NULL;
    client_state_t st;
    uint16_t sport;
    uint16_t cport;
    int rc;
    int failed = 0;

    sport = kind == ZRPC_TRANSPORT_TCP ? 42211 : 42111;
    cport = kind == ZRPC_TRANSPORT_TCP ? 42212 : 42112;

    memset(&scfg, 0, sizeof(scfg));
    scfg.name = "srv";
    scfg.transport = kind;
    scfg.bind_host = "127.0.0.1";
    scfg.data_port = 0;
    scfg.discovery_host = "127.0.0.1";
    scfg.discovery_port = sport;
    scfg.io_threads = 1;
    scfg.hello_interval_ms = 200;
    scfg.node_ttl_ms = 3000;

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.name = "cli";
    ccfg.transport = kind;
    ccfg.bind_host = "127.0.0.1";
    ccfg.data_port = 0;
    ccfg.discovery_host = "127.0.0.1";
    ccfg.discovery_port = cport;
    ccfg.io_threads = 1;
    ccfg.hello_interval_ms = 200;
    ccfg.node_ttl_ms = 3000;

    rc = zrpc_node_create(&scfg, &server);
    if (rc != ZRPC_OK) {
        printf("[%s] server create failed: %d\n", label, rc);
        return 1;
    }
    rc = zrpc_node_create(&ccfg, &client);
    if (rc != ZRPC_OK) {
        printf("[%s] client create failed: %d\n", label, rc);
        zrpc_node_destroy(server);
        return 1;
    }
    (void)zrpc_node_add_discovery_peer(server, "127.0.0.1", cport);
    (void)zrpc_node_add_discovery_peer(client, "127.0.0.1", sport);

    rc = zrpc_service_create(server, "echo", &svc);
    if (rc != ZRPC_OK || !svc) {
        printf("[%s] service create failed: %d\n", label, rc);
        failed = 1;
        goto cleanup;
    }
    rc = zrpc_service_add_method(svc, "ping", echo_handler, NULL);
    if (rc != ZRPC_OK) {
        printf("[%s] add method failed: %d\n", label, rc);
        failed = 1;
        goto cleanup;
    }
    rc = zrpc_proxy_create(client, NULL, &proxy);
    if (rc != ZRPC_OK || !proxy) {
        printf("[%s] proxy create failed: %d\n", label, rc);
        failed = 1;
        goto cleanup;
    }

    memset(&st, 0, sizeof(st));
    st.sem = ztk_sem_create(0);
    if (!st.sem) {
        failed = 1;
        goto cleanup;
    }

    /* 用例 1：单分片小消息。 */
    {
        const char *msg = "hello-zrpc";
        rc = run_call(proxy, &st, msg, strlen(msg) + 1);
        if (rc != ZRPC_OK || strcmp(st.buf, msg) != 0) {
            printf("[%s] small message failed: status=%d buf=%s\n", label, st.status, st.buf);
            failed = 1;
        }
    }
    /* 用例 2：多分片大消息（UDP 验证分片/重组；TCP 走流式分帧）。 */
    {
        static char big[5000];
        size_t i;
        for (i = 0; i < sizeof(big); i++) {
            big[i] = (char)('A' + (i % 26));
        }
        rc = run_call(proxy, &st, big, sizeof(big));
        if (rc != ZRPC_OK) {
            printf("[%s] big message failed: status=%d\n", label, st.status);
            failed = 1;
        } else if (st.len != sizeof(big) || memcmp(st.buf, big, sizeof(big)) != 0) {
            printf("[%s] big message mismatch: len=%zu\n", label, st.len);
            failed = 1;
        }
    }

    ztk_sem_destroy(st.sem);

    {
        zrpc_metrics_t metrics;
        memset(&metrics, 0, sizeof(metrics));
        zrpc_node_metrics(client, &metrics);
        if (metrics.requests_sent < 2 || metrics.responses_received < 2) {
            printf("[%s] metrics mismatch: sent=%llu recv=%llu\n", label,
                   (unsigned long long)metrics.requests_sent,
                   (unsigned long long)metrics.responses_received);
            failed = 1;
        }
        if (kind == ZRPC_TRANSPORT_UDP && metrics.udp_packets_sent == 0) {
            printf("[%s] udp metrics missing\n", label);
            failed = 1;
        }
    }

cleanup:
    if (proxy) {
        zrpc_proxy_destroy(proxy);
    }
    if (client) {
        zrpc_node_destroy(client);
    }
    if (server) {
        zrpc_node_destroy(server);
    }
    return failed;
}

int main(void) {
    int failed = 0;
    failed += run_case(ZRPC_TRANSPORT_UDP, "udp");
    failed += run_case(ZRPC_TRANSPORT_TCP, "tcp");

    if (failed) {
        printf("zrpc_loopback: FAIL\n");
        return 1;
    }
    printf("zrpc_loopback: OK\n");
    return 0;
}
