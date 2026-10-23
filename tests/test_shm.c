/*
 * SHM transport end-to-end test.
 *
 * Two nodes in one process share a real POSIX shared-memory channel. The
 * request/response rings and provider threads are exercised exactly as they
 * are for two processes. Cross-process coverage is provided by the benchmark
 * tools (`--shm`) and the quality gate.
 */
#include <client.h>
#include <server.h>
#include <shm.h>
#include <platform.h>
#include <thread/sem.h>

#define NB_LOG_TAG "test"
#include <log.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct shm_client_state {
    ntk_sem *sem;
    int status;
    uint8_t buf[8192];
    size_t len;
} shm_client_state_t;

static void echo_handler(nb_call_t *call, const nb_request_t *req, void *user)
{
    (void)user;
    nb_reply_bytes(call, 0, req->payload.data, req->payload.len);
}

static void on_response(int status, const nb_response_t *resp, void *user)
{
    shm_client_state_t *st = (shm_client_state_t *)user;
    st->status = status;
    st->len = 0;
    if (status == NB_OK && resp && resp->payload.len <= sizeof(st->buf)) {
        memcpy(st->buf, resp->payload.data, resp->payload.len);
        st->len = resp->payload.len;
    }
    ntk_sem_post(st->sem, 1);
}

static int run_call(nb_proxy_t *proxy, shm_client_state_t *st,
                    const void *data, size_t len)
{
    int attempt;

    for (attempt = 0; attempt < 50; attempt++) {
        nb_payload_t payload;
        int rc;

        payload.data = data;
        payload.len = len;
        payload.encoding = 0;
        st->status = -999;
        st->len = 0;
        rc = nb_proxy_call_async(
            proxy, "echo", "ping", &payload, on_response, st, 2000, NULL);
        if (rc != NB_OK) {
            return rc;
        }
        if (ntk_sem_timedwait(st->sem, 2500) == NTK_OK) {
            if (st->status == NB_ERR_AGAIN) {
                ntk_sleep_ms(10);
                continue;
            }
            return st->status;
        }
        ntk_sleep_ms(10);
    }
    return NB_ERR_TIMEOUT;
}

static int verify_call(nb_proxy_t *proxy, shm_client_state_t *st,
                       const void *data, size_t len)
{
    int rc = run_call(proxy, st, data, len);
    if (rc != NB_OK) {
        nb_log_error("shm call failed: %d", rc);
        return 1;
    }
    if (st->len != len || memcmp(st->buf, data, len) != 0) {
        nb_log_error("shm response mismatch: got=%zu expected=%zu", st->len, len);
        return 1;
    }
    return 0;
}

int main(void)
{
    char channel[64];
    char server_endpoint[96];
    char client_endpoint[96];
    nb_binding_config_t server_binding;
    nb_binding_config_t client_binding;
    nb_node_config_t server_cfg;
    nb_node_config_t client_cfg;
    nb_node_t *server = NULL;
    nb_node_t *client = NULL;
    nb_service_t *service = NULL;
    nb_proxy_t *proxy = NULL;
    shm_client_state_t st;
    int failed = 0;
    int rc;
    int i;

    snprintf(channel, sizeof(channel), "test-shm-%ld", (long)getpid());
    snprintf(server_endpoint, sizeof(server_endpoint), "shm://%s:1", channel);
    snprintf(client_endpoint, sizeof(client_endpoint), "shm://%s:2", channel);

    if (nb_shm_register() != NB_OK) {
        nb_log_error("shm provider register failed");
        return 1;
    }

    memset(&server_binding, 0, sizeof(server_binding));
    server_binding.name = "shm";
    server_binding.endpoint = server_endpoint;
    server_binding.intra_process = NB_INTRA_OFF;
    memset(&client_binding, 0, sizeof(client_binding));
    client_binding.name = "shm";
    client_binding.endpoint = client_endpoint;
    client_binding.intra_process = NB_INTRA_OFF;

    nb_node_config_default(&server_cfg);
    server_cfg.name = "shm-server";
    server_cfg.mode = NB_MODE_STATIC;
    server_cfg.runtime.io_threads = 1;
    server_cfg.transport.bindings = &server_binding;
    server_cfg.transport.binding_count = 1;

    nb_node_config_default(&client_cfg);
    client_cfg.name = "shm-client";
    client_cfg.mode = NB_MODE_STATIC;
    client_cfg.runtime.io_threads = 1;
    client_cfg.transport.bindings = &client_binding;
    client_cfg.transport.binding_count = 1;

    rc = nb_node_create(&server_cfg, &server);
    if (rc == NB_OK) {
        rc = nb_node_create(&client_cfg, &client);
    }
    if (rc == NB_OK) {
        rc = nb_service_create(server, "echo", &service);
    }
    if (rc == NB_OK) {
        rc = nb_service_add_method(service, "ping", echo_handler, NULL);
    }
    if (rc == NB_OK) {
        rc = nb_proxy_create(client, NULL, &proxy);
    }
    if (rc == NB_OK) {
        rc = nb_proxy_set_endpoint_uri(proxy, server_endpoint);
    }
    if (rc != NB_OK) {
        nb_log_error("shm setup failed: %d", rc);
        failed = 1;
        goto done;
    }

    memset(&st, 0, sizeof(st));
    st.sem = ntk_sem_create(0);
    if (!st.sem) {
        failed = 1;
        goto done;
    }

    {
        const char *small = "hello-shm";
        if (verify_call(proxy, &st, small, strlen(small) + 1)) {
            failed = 1;
        }
    }
    {
        uint8_t big[5000];
        for (i = 0; i < (int)sizeof(big); i++) {
            big[i] = (uint8_t)('A' + (i % 26));
        }
        if (verify_call(proxy, &st, big, sizeof(big))) {
            failed = 1;
        }
    }
    for (i = 0; i < 100 && !failed; i++) {
        char text[64];
        snprintf(text, sizeof(text), "shm-request-%d", i);
        if (verify_call(proxy, &st, text, strlen(text) + 1)) {
            failed = 1;
        }
    }

    ntk_sem_destroy(st.sem);

done:
    if (proxy) {
        nb_proxy_destroy(proxy);
    }
    if (client) {
        nb_node_destroy(client);
    }
    if (service) {
        nb_service_destroy(service);
    }
    if (server) {
        nb_node_destroy(server);
    }
    if (failed) {
        nb_log_error("nb_shm: FAIL");
        return 1;
    }
    nb_log_info("nb_shm: NB_OK");
    return 0;
}
