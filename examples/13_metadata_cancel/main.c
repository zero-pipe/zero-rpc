/*
 * 13 Metadata / Cancel: request metadata, deadline propagation, and cancel.
 */
#include <client.h>
#include <server.h>
#include <platform.h>

#define NB_LOG_TAG "example"
#include <log.h>

#include <stdio.h>
#include <string.h>

typedef struct client_state {
    int done;
    int status;
    char message[64];
    char meta[64];
} client_state_t;

static void echo_handler(nb_call_t *call, const nb_request_t *req, void *user)
{
    nb_metadata_t out;
    const char *client = req->metadata ? nb_metadata_get_string(req->metadata, "client") : NULL;
    (void)user;

    nb_metadata_init(&out);
    nb_metadata_set_string(&out, "server", "1");
    nb_log_info("server deadline=%llu ms, client=%s",
                (unsigned long long)nb_call_deadline_ms(call),
                client ? client : "(none)");
    nb_reply_ex(call, NB_OK, "metadata-ok", &req->payload, &out);
}

static void slow_handler(nb_call_t *call, const nb_request_t *req, void *user)
{
    (void)user;
    ntk_sleep_ms(500);
    nb_reply_bytes(call, NB_OK, req->payload.data, req->payload.len);
}

static int wait_done(nb_node_t *client, client_state_t *st)
{
    int i;

    for (i = 0; i < 50 && !st->done; ++i) {
        (void)nb_node_spin_once(client, 100);
    }
    return st->done ? NB_OK : NB_ERR_TIMEOUT;
}

static void on_echo(int status, const nb_response_t *resp, void *user)
{
    client_state_t *st = (client_state_t *)user;
    st->status = status;
    st->done = 1;
    if (status == NB_OK && resp) {
        const char *meta = resp->metadata ? nb_metadata_get_string(resp->metadata, "server") : NULL;
        if (resp->message) {
            snprintf(st->message, sizeof(st->message), "%s", resp->message);
        }
        if (meta) {
            snprintf(st->meta, sizeof(st->meta), "%s", meta);
        }
    }
}

static void on_slow(int status, const nb_response_t *resp, void *user)
{
    client_state_t *st = (client_state_t *)user;
    (void)resp;
    st->status = status;
    st->done = 1;
}

int main(void)
{
    nb_node_config_t scfg;
    nb_node_config_t ccfg;
    nb_node_t *server = NULL;
    nb_node_t *client = NULL;
    nb_service_t *svc = NULL;
    nb_proxy_t *proxy = NULL;
    nb_service_method_options_t options = NB_SERVICE_METHOD_OPTIONS_INIT;
    nb_metadata_t metadata;
    nb_payload_t request;
    client_state_t state;
    uint64_t request_id = 0;
    char endpoint[64];
    int rc;

    nb_node_config_default(&scfg);
    scfg.name = "SemanticsServer";
    scfg.mode = NB_MODE_STATIC;
    scfg.transport.transport = NB_TRANSPORT_TCP;
    scfg.transport.bind_host = "127.0.0.1";
    scfg.transport.data_port = 0;
    scfg.runtime.io_threads = 1;
    scfg.runtime.handler_threads = 2;

    nb_node_config_default(&ccfg);
    ccfg.name = "SemanticsClient";
    ccfg.mode = NB_MODE_STATIC;
    ccfg.transport.transport = NB_TRANSPORT_TCP;
    ccfg.transport.bind_host = "127.0.0.1";
    ccfg.transport.data_port = 0;
    ccfg.runtime.io_threads = 1;

    options.exec = NB_SERVICE_EXEC_WORKER;

    rc = nb_node_create(&scfg, &server);
    if (rc == NB_OK) {
        rc = nb_service_create(server, "SemanticsService", &svc);
    }
    if (rc == NB_OK) {
        rc = nb_service_add_method_ex(svc, "echo", echo_handler, NULL, &options);
    }
    if (rc == NB_OK) {
        rc = nb_service_add_method_ex(svc, "slow", slow_handler, NULL, &options);
    }
    if (rc == NB_OK) {
        rc = nb_node_create(&ccfg, &client);
    }
    if (rc == NB_OK) {
        rc = nb_proxy_create(client, NULL, &proxy);
    }
    if (rc == NB_OK) {
        snprintf(endpoint,
                 sizeof(endpoint),
                 "tcp://127.0.0.1:%u",
                 (unsigned)nb_node_data_port(server));
        rc = nb_proxy_set_endpoint_uri(proxy, endpoint);
    }
    if (rc != NB_OK) {
        nb_log_error("semantics setup failed: %d (%s)", rc, nb_error_name(rc));
        goto done;
    }

    nb_metadata_init(&metadata);
    nb_metadata_set_string(&metadata, "client", "example");
    request.data = "hello metadata";
    request.len = strlen((const char *)request.data);
    request.encoding = 0;
    memset(&state, 0, sizeof(state));

    rc = nb_proxy_call_async_ex(proxy,
                                "SemanticsService",
                                "echo",
                                &request,
                                &metadata,
                                on_echo,
                                &state,
                                2000,
                                NULL);
    if (rc == NB_OK) {
        rc = wait_done(client, &state);
    }
    if (rc == NB_OK && state.status == NB_OK) {
        nb_log_info("response status=%d message=%s meta.server=%s",
                    state.status,
                    state.message,
                    state.meta);
    } else {
        nb_log_error("metadata call failed: rc=%d status=%d", rc, state.status);
    }

    memset(&state, 0, sizeof(state));
    rc = nb_proxy_call_async(proxy,
                             "SemanticsService",
                             "slow",
                             &request,
                             on_slow,
                             &state,
                             3000,
                             &request_id);
    if (rc == NB_OK) {
        rc = nb_proxy_cancel(proxy, request_id);
    }
    if (rc == NB_OK) {
        rc = wait_done(client, &state);
    }
    if (rc == NB_OK && state.status == NB_ERR_NOTFOUND) {
        nb_log_info("cancel delivered as expected: %s", nb_error_name(state.status));
    } else {
        nb_log_error("cancel check failed: rc=%d status=%d", rc, state.status);
    }

done:
    nb_proxy_destroy(proxy);
    nb_node_destroy(client);
    nb_service_destroy(svc);
    nb_node_destroy(server);
    return rc == NB_OK ? 0 : 1;
}
