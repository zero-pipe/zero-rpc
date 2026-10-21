#include <client.h>
#include <server.h>
#include <platform.h>
#include <thread/sem.h>

#define RPC_LOG_TAG "test"
#include <log.h>

#include <stdio.h>
#include <string.h>

/* 服务端 handler：原样回显。 */
static void echo_handler(call_t *call, const request_t *req, void *user)
{
    (void)user;
    reply_bytes(call, 0, req->payload.data, req->payload.len);
}

typedef struct client_state {
    ntk_sem *sem;
    int status;
    char buf[8192];
    size_t len;
} client_state_t;

static void on_response(int status, const response_t *resp, void *user)
{
    client_state_t *st = (client_state_t *)user;
    st->status = status;
    if (status == OK && resp && resp->payload.len <= sizeof(st->buf)) {
        memcpy(st->buf, resp->payload.data, resp->payload.len);
        st->len = resp->payload.len;
        st->buf[resp->payload.len < sizeof(st->buf) ? resp->payload.len : sizeof(st->buf) - 1] =
            '\0';
    }
    ntk_sem_post(st->sem, 1);
}

static int wait_response(client_state_t *st, unsigned timeout_ms)
{
    if (ntk_sem_timedwait(st->sem, timeout_ms) != NTK_OK) {
        return -1;
    }
    return 0;
}

/* 反复尝试直到发现收敛或超时。返回最终 status。 */
static int run_call(proxy_t *proxy, client_state_t *st, const void *data, size_t len)
{
    int attempt;
    payload_t pl;
    pl.data = data;
    pl.len = len;
    pl.encoding = 0;
    for (attempt = 0; attempt < 40; attempt++) {
        int rc;
        st->status = -999;
        st->len = 0;
        rc = proxy_call_async(proxy, "echo", "ping", &pl, on_response, st, 1000, NULL);
        if (rc != OK) {
            return rc;
        }
        if (wait_response(st, 1000) == 0) {
            if (st->status == ERR_NOTFOUND) {
                ntk_sleep_ms(50);
                continue;
            }
            return st->status;
        }
        ntk_sleep_ms(50);
    }
    return -1;
}

static int run_case(transport_kind_t kind, const char *label)
{
    node_config_t scfg;
    node_config_t ccfg;
    node_t *server = NULL;
    node_t *client = NULL;
    service_t *svc = NULL;
    proxy_t *proxy = NULL;
    client_state_t st;
    uint16_t sport;
    uint16_t cport;
    int rc;
    int failed = 0;

    sport = kind == TRANSPORT_TCP ? 42211 : 42111;
    cport = kind == TRANSPORT_TCP ? 42212 : 42112;

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

    rc = node_create(&scfg, &server);
    if (rc != OK) {
        rpc_log_error("[%s] server create failed: %d", label, rc);
        return 1;
    }
    rc = node_create(&ccfg, &client);
    if (rc != OK) {
        rpc_log_error("[%s] client create failed: %d", label, rc);
        node_destroy(server);
        return 1;
    }
    (void)node_add_discovery_peer(server, "127.0.0.1", cport);
    (void)node_add_discovery_peer(client, "127.0.0.1", sport);

    rc = service_create(server, "echo", &svc);
    if (rc != OK || !svc) {
        rpc_log_error("[%s] service create failed: %d", label, rc);
        failed = 1;
        goto cleanup;
    }
    rc = service_add_method(svc, "ping", echo_handler, NULL);
    if (rc != OK) {
        rpc_log_error("[%s] add method failed: %d", label, rc);
        failed = 1;
        goto cleanup;
    }
    rc = proxy_create(client, NULL, &proxy);
    if (rc != OK || !proxy) {
        rpc_log_error("[%s] proxy create failed: %d", label, rc);
        failed = 1;
        goto cleanup;
    }

    memset(&st, 0, sizeof(st));
    st.sem = ntk_sem_create(0);
    if (!st.sem) {
        failed = 1;
        goto cleanup;
    }

    /* 用例 1：单分片小消息。 */
    {
        const char *msg = "hello-rpc";
        rc = run_call(proxy, &st, msg, strlen(msg) + 1);
        if (rc != OK || strcmp(st.buf, msg) != 0) {
            rpc_log_error("[%s] small message failed: status=%d buf=%s", label, st.status, st.buf);
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
        if (rc != OK) {
            rpc_log_error("[%s] big message failed: status=%d", label, st.status);
            failed = 1;
        } else if (st.len != sizeof(big) || memcmp(st.buf, big, sizeof(big)) != 0) {
            rpc_log_error("[%s] big message mismatch: len=%zu", label, st.len);
            failed = 1;
        }
    }

    ntk_sem_destroy(st.sem);

    {
        metrics_t metrics;
        memset(&metrics, 0, sizeof(metrics));
        node_metrics(client, &metrics);
        if (metrics.requests_sent < 2 || metrics.responses_received < 2) {
            rpc_log_error("[%s] metrics mismatch: sent=%llu recv=%llu",
                          label,
                          (unsigned long long)metrics.requests_sent,
                          (unsigned long long)metrics.responses_received);
            failed = 1;
        }
        if (kind == TRANSPORT_UDP && metrics.udp_packets_sent == 0) {
            rpc_log_error("[%s] udp metrics missing", label);
            failed = 1;
        }
    }

    /* RTCP：UDP 会话应在周期内双向交换 SR（本端发送方，故发 SR；对端也发 SR）。 */
    if (kind == TRANSPORT_UDP) {
        metrics_t m;
        int waited = 0;
        int ok = 0;
        memset(&m, 0, sizeof(m));
        while (waited < 4000) {
            node_metrics(client, &m);
            if (m.rtcp_sr_sent > 0 && m.rtcp_sr_received > 0) {
                ok = 1;
                break;
            }
            ntk_sleep_ms(200);
            waited += 200;
        }
        if (!ok) {
            rpc_log_error("[%s] rtcp sr not exchanged: sent=%llu recv=%llu",
                          label,
                          (unsigned long long)m.rtcp_sr_sent,
                          (unsigned long long)m.rtcp_sr_received);
            failed = 1;
        } else {
            char ep[128];
            endpoint_t e;
            peer_stats_t peer;
            if (node_binding_endpoint(server, 0, ep, sizeof(ep)) == OK &&
                endpoint_parse(ep, &e) == OK &&
                node_peer_stats(client, e.host, e.port, &peer) == OK) {
                if (peer.feedback_sent == 0) {
                    rpc_log_error("[%s] rtcp peer stats empty", label);
                    failed = 1;
                }
            } else {
                rpc_log_error("[%s] rtcp peer query failed", label);
                failed = 1;
            }
        }
    }

cleanup:
    if (proxy) {
        proxy_destroy(proxy);
    }
    if (client) {
        node_destroy(client);
    }
    if (server) {
        node_destroy(server);
    }
    return failed;
}

int main(void)
{
    int failed = 0;
    failed += run_case(TRANSPORT_UDP, "udp");
    failed += run_case(TRANSPORT_TCP, "tcp");

    if (failed) {
        rpc_log_error("zri_loopback: FAIL");
        return 1;
    }
    rpc_log_info("zri_loopback: OK");
    return 0;
}
