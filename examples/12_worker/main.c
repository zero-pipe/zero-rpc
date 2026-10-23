/*
 * 12 Worker: run a blocking handler on the node worker pool.
 *
 * The server configures handler_threads=2 and registers slow with
 * NB_SERVICE_EXEC_WORKER. The handler sleeps on a worker thread, not on the
 * poller thread that owns the transport.
 */
#include <client.h>
#include <server.h>
#include <platform.h>

#define NB_LOG_TAG "example"
#include <log.h>

#include <stdio.h>
#include <string.h>

static void slow_handler(nb_call_t *call, const nb_request_t *req, void *user)
{
    (void)user;
    ntk_sleep_ms(300);
    nb_reply_bytes(call, NB_OK, req->payload.data, req->payload.len);
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
    nb_response_t response;
    nb_payload_t request;
    char endpoint[64];
    int rc;

    nb_node_config_default(&scfg);
    scfg.name = "WorkerServer";
    scfg.mode = NB_MODE_STATIC;
    scfg.transport.transport = NB_TRANSPORT_TCP;
    scfg.transport.bind_host = "127.0.0.1";
    scfg.transport.data_port = 0;
    scfg.runtime.io_threads = 1;
    scfg.runtime.handler_threads = 2;

    nb_node_config_default(&ccfg);
    ccfg.name = "WorkerClient";
    ccfg.mode = NB_MODE_STATIC;
    ccfg.transport.transport = NB_TRANSPORT_TCP;
    ccfg.transport.bind_host = "127.0.0.1";
    ccfg.transport.data_port = 0;
    ccfg.runtime.io_threads = 1;

    options.exec = NB_SERVICE_EXEC_WORKER;

    rc = nb_node_create(&scfg, &server);
    if (rc == NB_OK) {
        rc = nb_service_create(server, "WorkerService", &svc);
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
        nb_log_error("worker setup failed: %d (%s)", rc, nb_error_name(rc));
        goto done;
    }

    request.data = "slow-request";
    request.len = strlen((const char *)request.data);
    request.encoding = 0;
    memset(&response, 0, sizeof(response));
    nb_log_info("calling worker handler, main thread will block for about 300ms");
    rc = nb_proxy_call_sync(proxy, "WorkerService", "slow", &request, &response, 3000, NULL);
    if (rc == NB_OK) {
        nb_log_info("worker response: %.*s",
                    (int)response.payload.len,
                    (const char *)response.payload.data);
    } else {
        nb_log_error("worker call failed: %d (%s)", rc, nb_error_name(rc));
    }
    nb_response_release(&response);

done:
    nb_proxy_destroy(proxy);
    nb_node_destroy(client);
    nb_service_destroy(svc);
    nb_node_destroy(server);
    return rc == NB_OK ? 0 : 1;
}
