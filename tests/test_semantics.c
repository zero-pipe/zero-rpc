/*
 * gRPC 语义对齐：metadata、rich status、服务端/客户端拦截器。
 */
#include <client.h>
#include <server.h>
#include <platform.h>
#include <thread/sem.h>

#define NB_LOG_TAG "test"
#include <log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct client_state {
    ntk_sem *sem;
    int status;
    int app_status;
    char message[64];
    char meta[64];
    char seen[64];
    char buf[256];
    size_t len;
} client_state_t;

static int g_server_interceptor_calls;
static int g_client_before_calls;
static int g_client_after_calls;

static int server_interceptor(nb_call_t *call, const nb_request_t *req, void *user)
{
    (void)call;
    (void)user;
    g_server_interceptor_calls++;
    if (strcmp(req->method, "deny") == 0) {
        return NB_ERR_AGAIN;
    }
    return NB_OK;
}

static void echo_meta_handler(nb_call_t *call, const nb_request_t *req, void *user)
{
    nb_metadata_t out;
    const char *k = req->metadata ? nb_metadata_get_string(req->metadata, "k") : NULL;
    (void)user;
    nb_metadata_init(&out);
    nb_metadata_set_string(&out, "resp", "ok");
    if (k) {
        nb_metadata_set_string(&out, "seen", k);
    }
    nb_reply_ex(call, 0, "done", &req->payload, &out);
}

static void on_response(int status, const nb_response_t *resp, void *user)
{
    client_state_t *st = (client_state_t *)user;
    st->status = status;
    st->app_status = resp ? resp->status : status;
    if (resp) {
        if (resp->message) {
            snprintf(st->message, sizeof(st->message), "%s", resp->message);
        }
        if (resp->metadata) {
            const char *m = nb_metadata_get_string(resp->metadata, "resp");
            const char *s = nb_metadata_get_string(resp->metadata, "seen");
            if (m)
                snprintf(st->meta, sizeof(st->meta), "%s", m);
            if (s)
                snprintf(st->seen, sizeof(st->seen), "%s", s);
        }
        if (resp->payload.len <= sizeof(st->buf)) {
            memcpy(st->buf, resp->payload.data, resp->payload.len);
            st->len = resp->payload.len;
        }
    }
    ntk_sem_post(st->sem, 1);
}

static int client_before(
    void *ctx, const char *service, const char *method, nb_metadata_t *md, uint64_t *timeout_ms)
{
    (void)ctx;
    (void)service;
    (void)method;
    (void)timeout_ms;
    g_client_before_calls++;
    return nb_metadata_set_string(md, "client", "1");
}

static void client_after(void *ctx, int status, const nb_response_t *resp)
{
    (void)ctx;
    (void)status;
    (void)resp;
    g_client_after_calls++;
}

static int test_metadata_unit(void)
{
    nb_metadata_t md;
    nb_metadata_t out;
    uint8_t wire[NB_METADATA_WIRE_MAX];
    size_t wire_len = 0;
    const char *v;
    int failed = 0;

    nb_metadata_init(&md);
    if (nb_metadata_set_string(&md, "a", "hello") != NB_OK ||
        nb_metadata_set(&md, "b", "\x01\x02\x00", 3) != NB_OK) {
        failed = 1;
    }
    if (nb_metadata_encode(&md, wire, sizeof(wire), &wire_len) != NB_OK || wire_len == 0) {
        failed = 1;
    }
    nb_metadata_init(&out);
    if (nb_metadata_decode(&out, wire, wire_len) != NB_OK) {
        failed = 1;
    }
    v = nb_metadata_get_string(&out, "a");
    if (!v || strcmp(v, "hello") != 0) {
        failed = 1;
    }
    {
        size_t blen = 0;
        const uint8_t *b = (const uint8_t *)nb_metadata_get(&out, "b", &blen);
        if (!b || blen != 3 || b[0] != 1 || b[2] != 0) {
            failed = 1;
        }
    }
    if (failed) {
        nb_log_error("[semantics] metadata unit failed");
    }
    return failed;
}

static int test_rpc_semantics(void)
{
    nb_node_config_t scfg;
    nb_node_config_t ccfg;
    nb_node_t *server = NULL;
    nb_node_t *client = NULL;
    nb_service_t *svc = NULL;
    nb_proxy_t *proxy = NULL;
    nb_proxy_interceptor_t interc;
    client_state_t ok;
    client_state_t denied;
    nb_metadata_t req_md;
    nb_payload_t pl;
    uint16_t sport = 46111;
    uint16_t cport = 46112;
    int failed = 0;

    memset(&ok, 0, sizeof(ok));
    memset(&denied, 0, sizeof(denied));
    memset(&interc, 0, sizeof(interc));
    interc.before = client_before;
    interc.after = client_after;

    memset(&scfg, 0, sizeof(scfg));
    scfg.name = "sem-srv";
    scfg.transport.transport = NB_TRANSPORT_TCP;
    scfg.transport.bind_host = "127.0.0.1";
    scfg.discovery.host = "127.0.0.1";
    scfg.discovery.port = sport;
    scfg.runtime.io_threads = 1;
    scfg.discovery.hello_interval_ms = 200;
    scfg.discovery.node_ttl_ms = 3000;

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.name = "sem-cli";
    ccfg.transport.transport = NB_TRANSPORT_TCP;
    ccfg.transport.bind_host = "127.0.0.1";
    ccfg.discovery.host = "127.0.0.1";
    ccfg.discovery.port = cport;
    ccfg.runtime.io_threads = 1;
    ccfg.discovery.hello_interval_ms = 200;
    ccfg.discovery.node_ttl_ms = 3000;

    ok.sem = ntk_sem_create(0);
    denied.sem = ntk_sem_create(0);
    if (!ok.sem || !denied.sem) {
        failed = 1;
        goto cleanup;
    }
    if (nb_node_create(&scfg, &server) != NB_OK || nb_node_create(&ccfg, &client) != NB_OK) {
        failed = 1;
        goto cleanup;
    }
    (void)nb_node_add_discovery_peer(server, "127.0.0.1", cport);
    (void)nb_node_add_discovery_peer(client, "127.0.0.1", sport);

    if (nb_service_create(server, "sem", &svc) != NB_OK ||
        nb_service_add_method(svc, "echo", echo_meta_handler, NULL) != NB_OK ||
        nb_service_add_method(svc, "deny", echo_meta_handler, NULL) != NB_OK ||
        nb_service_set_interceptor(svc, server_interceptor, NULL) != NB_OK) {
        nb_log_error("[semantics] service setup failed");
        failed = 1;
        goto cleanup;
    }
    if (nb_proxy_create(client, NULL, &proxy) != NB_OK || nb_proxy_set_interceptor(proxy, &interc) != NB_OK ||
        nb_node_wait_for_service(client, "sem", 5000) != NB_OK) {
        nb_log_error("[semantics] client setup failed");
        failed = 1;
        goto cleanup;
    }

    pl.data = "ping";
    pl.len = 5;
    pl.encoding = 0;
    nb_metadata_init(&req_md);
    nb_metadata_set_string(&req_md, "k", "v");

    if (nb_proxy_call_async_ex(proxy, "sem", "echo", &pl, &req_md, on_response, &ok, 3000, NULL) !=
        NB_OK) {
        failed = 1;
        goto cleanup;
    }
    if (ntk_sem_timedwait(ok.sem, 3000) != NTK_OK || ok.status != NB_OK) {
        nb_log_error("[semantics] echo status=%d", ok.status);
        failed = 1;
    } else if (ok.app_status != 0 || strcmp(ok.message, "done") != 0 ||
               strcmp(ok.meta, "ok") != 0 || strcmp(ok.seen, "v") != 0) {
        nb_log_error("[semantics] echo app=%d msg=%s resp=%s seen=%s",
                      ok.app_status,
                      ok.message,
                      ok.meta,
                      ok.seen);
        failed = 1;
    }

    if (nb_proxy_call_async_ex(proxy, "sem", "deny", &pl, NULL, on_response, &denied, 3000, NULL) !=
        NB_OK) {
        failed = 1;
        goto cleanup;
    }
    if (ntk_sem_timedwait(denied.sem, 3000) != NTK_OK) {
        failed = 1;
    } else if (denied.status != NB_OK || denied.app_status != NB_ERR_AGAIN) {
        nb_log_error("[semantics] deny app=%d (want %d)", denied.app_status, NB_ERR_AGAIN);
        failed = 1;
    }

    if (g_server_interceptor_calls != 2 || g_client_before_calls != 2 ||
        g_client_after_calls != 2) {
        nb_log_error("[semantics] interceptor calls srv=%d before=%d after=%d",
                      g_server_interceptor_calls,
                      g_client_before_calls,
                      g_client_after_calls);
        failed = 1;
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
    if (ok.sem) {
        ntk_sem_destroy(ok.sem);
    }
    if (denied.sem) {
        ntk_sem_destroy(denied.sem);
    }
    return failed;
}

int main(void)
{
    int failed = 0;
    failed += test_metadata_unit();
    failed += test_rpc_semantics();
    if (failed) {
        nb_log_error("semantics: FAIL");
        return 1;
    }
    nb_log_info("semantics: NB_OK");
    return 0;
}
