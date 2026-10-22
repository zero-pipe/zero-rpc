/* 覆盖新增抽象：service_table、message_view、stub 与 sync 便捷层。 */
#include <client.h>
#include <extensions.h>
#include <server.h>
#include <platform.h>

#define NB_LOG_TAG "test"
#include <log.h>

#include <stdio.h>
#include <string.h>

/* ---- 两端共享的声明表 ---- */
NB_SERVICE_DEFINE(
    g_order_table, 7u, "OrderService", NB_SERVICE_METHOD(1u, "get"), NB_SERVICE_METHOD(2u, "put"));

static int test_service_table(void)
{
    uint16_t id = 0;
    int failed = 0;
    nb_reset_service_table();
    if (nb_service_table_active()) {
        nb_log_error("table should be inactive after reset");
        failed = 1;
    }
    if (nb_install_service_table(&g_order_table) != NB_OK) {
        nb_log_error("install failed");
        return 1;
    }
    /* 幂等。 */
    if (nb_install_service_table(&g_order_table) != NB_OK) {
        nb_log_error("re-install should be idempotent");
        failed = 1;
    }
    if (!nb_service_table_active()) {
        nb_log_error("table should be active");
        failed = 1;
    }
    if (nb_service_table_find_id("OrderService", "get", &id) != NB_OK || id != 1u) {
        nb_log_error("find get failed: id=%u", (unsigned)id);
        failed = 1;
    }
    if (nb_service_table_find_id("OrderService", "missing", &id) != NB_ERR_NOTFOUND) {
        nb_log_error("missing method should not be found");
        failed = 1;
    }
    nb_reset_service_table();
    return failed;
}

static int test_message_view(void)
{
    nb_message_view_t view;
    nb_payload_t pl;
    int failed = 0;
    const char borrowed[] = "borrowed";
    nb_message_view_init(&view);
    if (nb_message_view_set_string(&view, "hello") != NB_OK || view.len != 5 ||
        memcmp(view.data, "hello", 5) != 0) {
        nb_log_error("set_string failed");
        failed = 1;
    }
    nb_message_view_release(&view);
    if (view.data != NULL || view.len != 0 || view.owned != NULL) {
        nb_log_error("release did not clear view");
        failed = 1;
    }
    if (nb_message_view_set_data(&view, "abc", 3) != NB_OK) {
        nb_log_error("set_data failed");
        failed = 1;
    }
    nb_message_view_set_borrowed(&view, borrowed, sizeof(borrowed) - 1);
    if (view.owned != NULL) {
        nb_log_error("borrowed view should not own memory");
        failed = 1;
    }
    pl.data = view.data;
    pl.len = view.len;
    pl.encoding = 2;
    if (nb_message_view_set_payload(&view, &pl) != NB_OK || view.encoding != 2 ||
        memcmp(view.data, borrowed, sizeof(borrowed) - 1) != 0) {
        nb_log_error("set_payload failed");
        failed = 1;
    }
    nb_message_view_release(&view);
    return failed;
}

static int echo_sync(const nb_request_t *req, nb_message_view_t *resp, void *user)
{
    int *calls = (int *)user;
    if (calls) {
        (*calls)++;
    }
    (void)nb_message_view_set_data(resp, req->payload.data, req->payload.len);
    return 0;
}

static int test_stub_and_sync(void)
{
    nb_node_config_t scfg;
    nb_node_config_t ccfg;
    nb_node_t *server = NULL;
    nb_node_t *client = NULL;
    nb_service_t *svc = NULL;
    nb_proxy_t *proxy = NULL;
    nb_stub_t *stub = NULL;
    nb_response_t response;
    nb_payload_t pl;
    uint16_t sport;
    uint16_t cport;
    int calls = 0;
    int failed = 0;
    int rc;

    sport = 42311;
    cport = 42312;

    memset(&scfg, 0, sizeof(scfg));
    scfg.name = "api_srv";
    scfg.transport.transport = NB_TRANSPORT_UDP;
    scfg.transport.bind_host = "127.0.0.1";
    scfg.discovery.host = "127.0.0.1";
    scfg.discovery.port = sport;
    scfg.runtime.io_threads = 1;
    scfg.discovery.hello_interval_ms = 200;
    scfg.discovery.node_ttl_ms = 3000;
    scfg.service_table = &g_order_table;

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.name = "api_cli";
    ccfg.transport.transport = NB_TRANSPORT_UDP;
    ccfg.transport.bind_host = "127.0.0.1";
    ccfg.discovery.host = "127.0.0.1";
    ccfg.discovery.port = cport;
    ccfg.runtime.io_threads = 1;
    ccfg.discovery.hello_interval_ms = 200;
    ccfg.discovery.node_ttl_ms = 3000;

    rc = nb_node_create(&scfg, &server);
    if (rc != NB_OK) {
        nb_log_error("server create failed: %d", rc);
        return 1;
    }
    rc = nb_service_create(server, "OrderService", &svc);
    if (rc == NB_OK) {
        rc = nb_service_add_sync_method(svc, "get", echo_sync, &calls);
    }
    if (rc != NB_OK) {
        nb_log_error("service setup failed: %d", rc);
        nb_node_destroy(server);
        return 1;
    }
    rc = nb_node_create(&ccfg, &client);
    if (rc != NB_OK) {
        nb_log_error("client create failed: %d", rc);
        nb_node_destroy(server);
        return 1;
    }
    (void)nb_node_add_discovery_peer(server, "127.0.0.1", cport);
    (void)nb_node_add_discovery_peer(client, "127.0.0.1", sport);

    if (nb_node_wait_for_service(client, "OrderService", 5000) != NB_OK) {
        nb_log_error("service not discovered");
        failed = 1;
        goto cleanup;
    }

    /* stub 低层原语 + sync 便捷层（proxy 现在在 stub 之上）。 */
    rc = nb_stub_create(client, NULL, &stub);
    if (rc != NB_OK || !stub) {
        nb_log_error("stub create failed: %d", rc);
        failed = 1;
        goto cleanup;
    }
    if (nb_stub_node(stub) != client) {
        nb_log_error("stub node mismatch");
        failed = 1;
    }

    rc = nb_proxy_create(client, NULL, &proxy);
    if (rc != NB_OK) {
        nb_log_error("proxy create failed: %d", rc);
        failed = 1;
        goto cleanup;
    }
    memset(&response, 0, sizeof(response));
    pl.data = "ping";
    pl.len = 4;
    pl.encoding = 0;
    rc = nb_proxy_call_sync(proxy, "OrderService", "get", &pl, &response, 3000, NULL);
    if (rc != NB_OK) {
        nb_log_error("call_sync failed: %d", rc);
        failed = 1;
    } else if (response.status != 0 || response.payload.len != 4 ||
               memcmp(response.payload.data, "ping", 4) != 0) {
        nb_log_error(
            "call_sync bad response: status=%d len=%zu", response.status, response.payload.len);
        failed = 1;
    }
    nb_response_release(&response);

    /* proxy 的单向事件入口：同一 node 既可作为 service 也可作为 client。 */
    {
        int before = calls;
        uint64_t event_id = 0;
        rc = nb_proxy_call_event(proxy, "OrderService", "get", &pl, &event_id);
        if (rc != NB_OK || event_id == 0) {
            nb_log_error("call_event failed: %d", rc);
            failed = 1;
        } else {
            ntk_sleep_ms(100);
            if (calls <= before) {
                nb_log_error("call_event was not delivered");
                failed = 1;
            }
        }
    }

cleanup:
    if (stub) {
        nb_stub_destroy(stub);
    }
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
    failed += test_service_table();
    failed += test_message_view();
    failed += test_stub_and_sync();
    if (failed) {
        nb_log_error("nb_api: FAIL");
        return 1;
    }
    nb_log_info("nb_api: NB_OK");
    return 0;
}
