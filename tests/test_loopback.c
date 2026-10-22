#include <client.h>
#include <server.h>
#include <platform.h>
#include <thread/sem.h>

#define NB_LOG_TAG "test"
#include <log.h>

#include <stdio.h>
#include <string.h>

/* 服务端 handler：原样回显。 */
static void echo_handler(nb_call_t *call, const nb_request_t *req, void *user)
{
    (void)user;
    nb_reply_bytes(call, 0, req->payload.data, req->payload.len);
}

typedef struct client_state {
    ntk_sem *sem;
    int status;
    char buf[8192];
    size_t len;
} client_state_t;

static void on_response(int status, const nb_response_t *resp, void *user)
{
    client_state_t *st = (client_state_t *)user;
    st->status = status;
    if (status == NB_OK && resp && resp->payload.len <= sizeof(st->buf)) {
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
static int run_call(nb_proxy_t *proxy, client_state_t *st, const void *data, size_t len)
{
    int attempt;
    nb_payload_t pl;
    pl.data = data;
    pl.len = len;
    pl.encoding = 0;
    for (attempt = 0; attempt < 40; attempt++) {
        int rc;
        st->status = -999;
        st->len = 0;
        rc = nb_proxy_call_async(proxy, "echo", "ping", &pl, on_response, st, 1000, NULL);
        if (rc != NB_OK) {
            return rc;
        }
        if (wait_response(st, 1000) == 0) {
            if (st->status == NB_ERR_NOTFOUND) {
                ntk_sleep_ms(50);
                continue;
            }
            return st->status;
        }
        ntk_sleep_ms(50);
    }
    return -1;
}

static int run_case(nb_transport_kind_t kind, const char *label)
{
    nb_node_config_t scfg;
    nb_node_config_t ccfg;
    nb_node_t *server = NULL;
    nb_node_t *client = NULL;
    nb_service_t *svc = NULL;
    nb_proxy_t *proxy = NULL;
    client_state_t st;
    uint16_t sport;
    uint16_t cport;
    int rc;
    int failed = 0;

    sport = kind == NB_TRANSPORT_TCP ? 42211 : 42111;
    cport = kind == NB_TRANSPORT_TCP ? 42212 : 42112;

    memset(&scfg, 0, sizeof(scfg));
    scfg.name = "srv";
    scfg.transport.transport = kind;
    scfg.transport.bind_host = "127.0.0.1";
    scfg.transport.data_port = 0;
    scfg.discovery.host = "127.0.0.1";
    scfg.discovery.port = sport;
    scfg.runtime.io_threads = 1;
    scfg.discovery.hello_interval_ms = 200;
    scfg.discovery.node_ttl_ms = 3000;

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.name = "cli";
    ccfg.transport.transport = kind;
    ccfg.transport.bind_host = "127.0.0.1";
    ccfg.transport.data_port = 0;
    ccfg.discovery.host = "127.0.0.1";
    ccfg.discovery.port = cport;
    ccfg.runtime.io_threads = 1;
    ccfg.discovery.hello_interval_ms = 200;
    ccfg.discovery.node_ttl_ms = 3000;

    rc = nb_node_create(&scfg, &server);
    if (rc != NB_OK) {
        nb_log_error("[%s] server create failed: %d", label, rc);
        return 1;
    }
    rc = nb_node_create(&ccfg, &client);
    if (rc != NB_OK) {
        nb_log_error("[%s] client create failed: %d", label, rc);
        nb_node_destroy(server);
        return 1;
    }
    (void)nb_node_add_discovery_peer(server, "127.0.0.1", cport);
    (void)nb_node_add_discovery_peer(client, "127.0.0.1", sport);

    rc = nb_service_create(server, "echo", &svc);
    if (rc != NB_OK || !svc) {
        nb_log_error("[%s] service create failed: %d", label, rc);
        failed = 1;
        goto cleanup;
    }
    rc = nb_service_add_method(svc, "ping", echo_handler, NULL);
    if (rc != NB_OK) {
        nb_log_error("[%s] add method failed: %d", label, rc);
        failed = 1;
        goto cleanup;
    }
    rc = nb_proxy_create(client, NULL, &proxy);
    if (rc != NB_OK || !proxy) {
        nb_log_error("[%s] proxy create failed: %d", label, rc);
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
        if (rc != NB_OK || strcmp(st.buf, msg) != 0) {
            nb_log_error("[%s] small message failed: status=%d buf=%s", label, st.status, st.buf);
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
        if (rc != NB_OK) {
            nb_log_error("[%s] big message failed: status=%d", label, st.status);
            failed = 1;
        } else if (st.len != sizeof(big) || memcmp(st.buf, big, sizeof(big)) != 0) {
            nb_log_error("[%s] big message mismatch: len=%zu", label, st.len);
            failed = 1;
        }
    }

    ntk_sem_destroy(st.sem);

    {
        nb_metrics_t metrics;
        memset(&metrics, 0, sizeof(metrics));
        nb_node_metrics(client, &metrics);
        if (metrics.requests_sent < 2 || metrics.responses_received < 2) {
            nb_log_error("[%s] metrics mismatch: sent=%llu recv=%llu",
                          label,
                          (unsigned long long)metrics.requests_sent,
                          (unsigned long long)metrics.responses_received);
            failed = 1;
        }
        if (kind == NB_TRANSPORT_UDP && metrics.udp_packets_sent == 0) {
            nb_log_error("[%s] udp metrics missing", label);
            failed = 1;
        }
        /* 端到端延迟应已采样，且分位值非零。 */
        if (metrics.latency_count < 2 || metrics.latency_p50_ms == 0) {
            nb_log_error("[%s] latency metrics missing: count=%llu p50=%llu",
                          label,
                          (unsigned long long)metrics.latency_count,
                          (unsigned long long)metrics.latency_p50_ms);
            failed = 1;
        }
        /* 活跃连接数：回环后客户端至少持有一条链路/会话。 */
        if (metrics.connections == 0) {
            nb_log_error("[%s] connections gauge missing", label);
            failed = 1;
        }
    }

    /* RTCP：UDP 会话应在周期内双向交换 SR（本端发送方，故发 SR；对端也发 SR）。 */
    if (kind == NB_TRANSPORT_UDP) {
        nb_metrics_t m;
        int waited = 0;
        int ok = 0;
        memset(&m, 0, sizeof(m));
        while (waited < 4000) {
            nb_node_metrics(client, &m);
            if (m.rtcp_sr_sent > 0 && m.rtcp_sr_received > 0) {
                ok = 1;
                break;
            }
            ntk_sleep_ms(200);
            waited += 200;
        }
        if (!ok) {
            nb_log_error("[%s] rtcp sr not exchanged: sent=%llu recv=%llu",
                          label,
                          (unsigned long long)m.rtcp_sr_sent,
                          (unsigned long long)m.rtcp_sr_received);
            failed = 1;
        } else {
            char ep[128];
            nb_endpoint_t e;
            nb_peer_stats_t peer;
            if (nb_node_binding_endpoint(server, 0, ep, sizeof(ep)) == NB_OK &&
                nb_endpoint_parse(ep, &e) == NB_OK &&
                nb_node_peer_stats(client, e.host, e.port, &peer) == NB_OK) {
                if (peer.feedback_sent == 0) {
                    nb_log_error("[%s] rtcp peer stats empty", label);
                    failed = 1;
                }
            } else {
                nb_log_error("[%s] rtcp peer query failed", label);
                failed = 1;
            }
        }
    }

cleanup:
    if (proxy) {
        nb_proxy_destroy(proxy);
    }
    if (client) {
        nb_node_destroy(client);
    }
    if (server) {
        nb_node_destroy(server);
    }
    return failed;
}

int main(void)
{
    int failed = 0;
    failed += run_case(NB_TRANSPORT_UDP, "udp");
    failed += run_case(NB_TRANSPORT_TCP, "tcp");

    if (failed) {
        nb_log_error("nb_loopback: FAIL");
        return 1;
    }
    nb_log_info("nb_loopback: NB_OK");
    return 0;
}
