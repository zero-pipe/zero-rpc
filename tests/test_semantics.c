/*
 * gRPC 语义对齐：metadata、rich status、服务端/客户端拦截器。
 */
#include <client.h>
#include <server.h>
#include <platform.h>
#include <thread/sem.h>

#define RPC_LOG_TAG "test"
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

static int server_interceptor(call_t *call, const request_t *req, void *user) {
    (void)call;
    (void)user;
    g_server_interceptor_calls++;
    if (strcmp(req->method, "deny") == 0) {
        return ERR_AGAIN;
    }
    return OK;
}

static void echo_meta_handler(call_t *call, const request_t *req, void *user) {
    metadata_t out;
    const char *k = req->metadata ? metadata_get_string(req->metadata, "k") : NULL;
    (void)user;
    metadata_init(&out);
    metadata_set_string(&out, "resp", "ok");
    if (k) {
        metadata_set_string(&out, "seen", k);
    }
    reply_ex(call, 0, "done", &req->payload, &out);
}

static void on_response(int status, const response_t *resp, void *user) {
    client_state_t *st = (client_state_t *)user;
    st->status = status;
    st->app_status = resp ? resp->status : status;
    if (resp) {
        if (resp->message) {
            snprintf(st->message, sizeof(st->message), "%s", resp->message);
        }
        if (resp->metadata) {
            const char *m = metadata_get_string(resp->metadata, "resp");
            const char *s = metadata_get_string(resp->metadata, "seen");
            if (m) snprintf(st->meta, sizeof(st->meta), "%s", m);
            if (s) snprintf(st->seen, sizeof(st->seen), "%s", s);
        }
        if (resp->payload.len <= sizeof(st->buf)) {
            memcpy(st->buf, resp->payload.data, resp->payload.len);
            st->len = resp->payload.len;
        }
    }
    ntk_sem_post(st->sem, 1);
}

static int client_before(void *ctx, const char *service, const char *method, metadata_t *md,
                         uint64_t *timeout_ms) {
    (void)ctx;
    (void)service;
    (void)method;
    (void)timeout_ms;
    g_client_before_calls++;
    return metadata_set_string(md, "client", "1");
}

static void client_after(void *ctx, int status, const response_t *resp) {
    (void)ctx;
    (void)status;
    (void)resp;
    g_client_after_calls++;
}

static int test_metadata_unit(void) {
    metadata_t md;
    metadata_t out;
    uint8_t wire[METADATA_WIRE_MAX];
    size_t wire_len = 0;
    const char *v;
    int failed = 0;

    metadata_init(&md);
    if (metadata_set_string(&md, "a", "hello") != OK ||
        metadata_set(&md, "b", "\x01\x02\x00", 3) != OK) {
        failed = 1;
    }
    if (metadata_encode(&md, wire, sizeof(wire), &wire_len) != OK || wire_len == 0) {
        failed = 1;
    }
    metadata_init(&out);
    if (metadata_decode(&out, wire, wire_len) != OK) {
        failed = 1;
    }
    v = metadata_get_string(&out, "a");
    if (!v || strcmp(v, "hello") != 0) {
        failed = 1;
    }
    {
        size_t blen = 0;
        const uint8_t *b = (const uint8_t *)metadata_get(&out, "b", &blen);
        if (!b || blen != 3 || b[0] != 1 || b[2] != 0) {
            failed = 1;
        }
    }
    if (failed) {
        rpc_log_error("[semantics] metadata unit failed");
    }
    return failed;
}

static int test_rpc_semantics(void) {
    node_config_t scfg;
    node_config_t ccfg;
    node_t *server = NULL;
    node_t *client = NULL;
    service_t *svc = NULL;
    proxy_t *proxy = NULL;
    proxy_interceptor_t interc;
    client_state_t ok;
    client_state_t denied;
    metadata_t req_md;
    payload_t pl;
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
    scfg.transport = TRANSPORT_TCP;
    scfg.bind_host = "127.0.0.1";
    scfg.discovery_host = "127.0.0.1";
    scfg.discovery_port = sport;
    scfg.io_threads = 1;
    scfg.hello_interval_ms = 200;
    scfg.node_ttl_ms = 3000;

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.name = "sem-cli";
    ccfg.transport = TRANSPORT_TCP;
    ccfg.bind_host = "127.0.0.1";
    ccfg.discovery_host = "127.0.0.1";
    ccfg.discovery_port = cport;
    ccfg.io_threads = 1;
    ccfg.hello_interval_ms = 200;
    ccfg.node_ttl_ms = 3000;

    ok.sem = ntk_sem_create(0);
    denied.sem = ntk_sem_create(0);
    if (!ok.sem || !denied.sem) {
        failed = 1;
        goto cleanup;
    }
    if (node_create(&scfg, &server) != OK || node_create(&ccfg, &client) != OK) {
        failed = 1;
        goto cleanup;
    }
    (void)node_add_discovery_peer(server, "127.0.0.1", cport);
    (void)node_add_discovery_peer(client, "127.0.0.1", sport);

    if (service_create(server, "sem", &svc) != OK ||
        service_add_method(svc, "echo", echo_meta_handler, NULL) != OK ||
        service_add_method(svc, "deny", echo_meta_handler, NULL) != OK ||
        service_set_interceptor(svc, server_interceptor, NULL) != OK) {
        rpc_log_error("[semantics] service setup failed");
        failed = 1;
        goto cleanup;
    }
    if (proxy_create(client, NULL, &proxy) != OK ||
        proxy_set_interceptor(proxy, &interc) != OK ||
        node_wait_for_service(client, "sem", 5000) != OK) {
        rpc_log_error("[semantics] client setup failed");
        failed = 1;
        goto cleanup;
    }

    pl.data = "ping";
    pl.len = 5;
    pl.encoding = 0;
    metadata_init(&req_md);
    metadata_set_string(&req_md, "k", "v");

    if (proxy_call_async_ex(proxy, "sem", "echo", &pl, &req_md, on_response, &ok, 3000, NULL) !=
        OK) {
        failed = 1;
        goto cleanup;
    }
    if (ntk_sem_timedwait(ok.sem, 3000) != NTK_OK || ok.status != OK) {
        rpc_log_error("[semantics] echo status=%d", ok.status);
        failed = 1;
    } else if (ok.app_status != 0 || strcmp(ok.message, "done") != 0 ||
               strcmp(ok.meta, "ok") != 0 || strcmp(ok.seen, "v") != 0) {
        rpc_log_error("[semantics] echo app=%d msg=%s resp=%s seen=%s", ok.app_status, ok.message,
                      ok.meta, ok.seen);
        failed = 1;
    }

    if (proxy_call_async_ex(proxy, "sem", "deny", &pl, NULL, on_response, &denied, 3000, NULL) !=
        OK) {
        failed = 1;
        goto cleanup;
    }
    if (ntk_sem_timedwait(denied.sem, 3000) != NTK_OK) {
        failed = 1;
    } else if (denied.status != OK || denied.app_status != ERR_AGAIN) {
        rpc_log_error("[semantics] deny app=%d (want %d)", denied.app_status, ERR_AGAIN);
        failed = 1;
    }

    if (g_server_interceptor_calls != 2 || g_client_before_calls != 2 ||
        g_client_after_calls != 2) {
        rpc_log_error("[semantics] interceptor calls srv=%d before=%d after=%d",
                      g_server_interceptor_calls, g_client_before_calls, g_client_after_calls);
        failed = 1;
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
    if (ok.sem) {
        ntk_sem_destroy(ok.sem);
    }
    if (denied.sem) {
        ntk_sem_destroy(denied.sem);
    }
    return failed;
}

int main(void) {
    int failed = 0;
    failed += test_metadata_unit();
    failed += test_rpc_semantics();
    if (failed) {
        rpc_log_error("semantics: FAIL");
        return 1;
    }
    rpc_log_info("semantics: OK");
    return 0;
}
