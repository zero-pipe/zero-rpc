/* 覆盖新增抽象：service_table、message_view、stub 与 sync 便捷层。 */
#include <client.h>
#include <extensions.h>
#include <server.h>
#include <platform.h>

#define RPC_LOG_TAG "test"
#include <log.h>

#include <stdio.h>
#include <string.h>

/* ---- 两端共享的声明表 ---- */
SERVICE_DEFINE(
    g_order_table, 7u, "OrderService", SERVICE_METHOD(1u, "get"), SERVICE_METHOD(2u, "put"));

static int test_service_table(void)
{
    uint16_t id = 0;
    int failed = 0;
    reset_service_table();
    if (service_table_active()) {
        rpc_log_error("table should be inactive after reset");
        failed = 1;
    }
    if (install_service_table(&g_order_table) != OK) {
        rpc_log_error("install failed");
        return 1;
    }
    /* 幂等。 */
    if (install_service_table(&g_order_table) != OK) {
        rpc_log_error("re-install should be idempotent");
        failed = 1;
    }
    if (!service_table_active()) {
        rpc_log_error("table should be active");
        failed = 1;
    }
    if (service_table_find_id("OrderService", "get", &id) != OK || id != 1u) {
        rpc_log_error("find get failed: id=%u", (unsigned)id);
        failed = 1;
    }
    if (service_table_find_id("OrderService", "missing", &id) != ERR_NOTFOUND) {
        rpc_log_error("missing method should not be found");
        failed = 1;
    }
    reset_service_table();
    return failed;
}

static int test_message_view(void)
{
    message_view_t view;
    payload_t pl;
    int failed = 0;
    const char borrowed[] = "borrowed";
    message_view_init(&view);
    if (message_view_set_string(&view, "hello") != OK || view.len != 5 ||
        memcmp(view.data, "hello", 5) != 0) {
        rpc_log_error("set_string failed");
        failed = 1;
    }
    message_view_release(&view);
    if (view.data != NULL || view.len != 0 || view.owned != NULL) {
        rpc_log_error("release did not clear view");
        failed = 1;
    }
    if (message_view_set_data(&view, "abc", 3) != OK) {
        rpc_log_error("set_data failed");
        failed = 1;
    }
    message_view_set_borrowed(&view, borrowed, sizeof(borrowed) - 1);
    if (view.owned != NULL) {
        rpc_log_error("borrowed view should not own memory");
        failed = 1;
    }
    pl.data = view.data;
    pl.len = view.len;
    pl.encoding = 2;
    if (message_view_set_payload(&view, &pl) != OK || view.encoding != 2 ||
        memcmp(view.data, borrowed, sizeof(borrowed) - 1) != 0) {
        rpc_log_error("set_payload failed");
        failed = 1;
    }
    message_view_release(&view);
    return failed;
}

static int echo_sync(const request_t *req, message_view_t *resp, void *user)
{
    int *calls = (int *)user;
    if (calls) {
        (*calls)++;
    }
    (void)message_view_set_data(resp, req->payload.data, req->payload.len);
    return 0;
}

static int test_stub_and_sync(void)
{
    node_config_t scfg;
    node_config_t ccfg;
    node_t *server = NULL;
    node_t *client = NULL;
    service_t *svc = NULL;
    proxy_t *proxy = NULL;
    stub_t *stub = NULL;
    response_t response;
    payload_t pl;
    uint16_t sport;
    uint16_t cport;
    int calls = 0;
    int failed = 0;
    int rc;

    sport = 42311;
    cport = 42312;

    memset(&scfg, 0, sizeof(scfg));
    scfg.name = "api_srv";
    scfg.transport = TRANSPORT_UDP;
    scfg.bind_host = "127.0.0.1";
    scfg.discovery_host = "127.0.0.1";
    scfg.discovery_port = sport;
    scfg.io_threads = 1;
    scfg.hello_interval_ms = 200;
    scfg.node_ttl_ms = 3000;
    scfg.service_table = &g_order_table;

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.name = "api_cli";
    ccfg.transport = TRANSPORT_UDP;
    ccfg.bind_host = "127.0.0.1";
    ccfg.discovery_host = "127.0.0.1";
    ccfg.discovery_port = cport;
    ccfg.io_threads = 1;
    ccfg.hello_interval_ms = 200;
    ccfg.node_ttl_ms = 3000;

    rc = node_create(&scfg, &server);
    if (rc != OK) {
        rpc_log_error("server create failed: %d", rc);
        return 1;
    }
    rc = service_create(server, "OrderService", &svc);
    if (rc == OK) {
        rc = service_add_sync_method(svc, "get", echo_sync, &calls);
    }
    if (rc != OK) {
        rpc_log_error("service setup failed: %d", rc);
        node_destroy(server);
        return 1;
    }
    rc = node_create(&ccfg, &client);
    if (rc != OK) {
        rpc_log_error("client create failed: %d", rc);
        node_destroy(server);
        return 1;
    }
    (void)node_add_discovery_peer(server, "127.0.0.1", cport);
    (void)node_add_discovery_peer(client, "127.0.0.1", sport);

    if (node_wait_for_service(client, "OrderService", 5000) != OK) {
        rpc_log_error("service not discovered");
        failed = 1;
        goto cleanup;
    }

    /* stub 低层原语 + sync 便捷层（proxy 现在在 stub 之上）。 */
    rc = stub_create(client, NULL, &stub);
    if (rc != OK || !stub) {
        rpc_log_error("stub create failed: %d", rc);
        failed = 1;
        goto cleanup;
    }
    if (stub_node(stub) != client) {
        rpc_log_error("stub node mismatch");
        failed = 1;
    }

    rc = proxy_create(client, NULL, &proxy);
    if (rc != OK) {
        rpc_log_error("proxy create failed: %d", rc);
        failed = 1;
        goto cleanup;
    }
    memset(&response, 0, sizeof(response));
    pl.data = "ping";
    pl.len = 4;
    pl.encoding = 0;
    rc = proxy_call_sync(proxy, "OrderService", "get", &pl, &response, 3000, NULL);
    if (rc != OK) {
        rpc_log_error("call_sync failed: %d", rc);
        failed = 1;
    } else if (response.status != 0 || response.payload.len != 4 ||
               memcmp(response.payload.data, "ping", 4) != 0) {
        rpc_log_error(
            "call_sync bad response: status=%d len=%zu", response.status, response.payload.len);
        failed = 1;
    }
    response_release(&response);

    /* proxy 的单向事件入口：同一 node 既可作为 service 也可作为 client。 */
    {
        int before = calls;
        uint64_t event_id = 0;
        rc = proxy_call_event(proxy, "OrderService", "get", &pl, &event_id);
        if (rc != OK || event_id == 0) {
            rpc_log_error("call_event failed: %d", rc);
            failed = 1;
        } else {
            ntk_sleep_ms(100);
            if (calls <= before) {
                rpc_log_error("call_event was not delivered");
                failed = 1;
            }
        }
    }

cleanup:
    if (stub) {
        stub_destroy(stub);
    }
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
    failed += test_service_table();
    failed += test_message_view();
    failed += test_stub_and_sync();
    if (failed) {
        rpc_log_error("zri_api: FAIL");
        return 1;
    }
    rpc_log_info("zri_api: OK");
    return 0;
}
