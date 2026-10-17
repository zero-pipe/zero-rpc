/* 覆盖新增抽象：service_table、message_view、stub 与 sync 便捷层。 */
#include <zrpc/zrpc_client.h>
#include <zrpc/zrpc_extensions.h>
#include <zrpc/zrpc_server.h>
#include <ztk/platform.h>

#include <stdio.h>
#include <string.h>

/* ---- 两端共享的声明表 ---- */
ZRPC_SERVICE_DEFINE(g_order_table, 7u, "OrderService",
                    ZRPC_SERVICE_METHOD(1u, "get"),
                    ZRPC_SERVICE_METHOD(2u, "put"));

static int test_service_table(void) {
    uint16_t id = 0;
    int failed = 0;
    zrpc_reset_service_table();
    if (zrpc_service_table_active()) {
        printf("table should be inactive after reset\n");
        failed = 1;
    }
    if (zrpc_install_service_table(&g_order_table) != ZRPC_OK) {
        printf("install failed\n");
        return 1;
    }
    /* 幂等。 */
    if (zrpc_install_service_table(&g_order_table) != ZRPC_OK) {
        printf("re-install should be idempotent\n");
        failed = 1;
    }
    if (!zrpc_service_table_active()) {
        printf("table should be active\n");
        failed = 1;
    }
    if (zrpc_service_table_find_id("OrderService", "get", &id) != ZRPC_OK || id != 1u) {
        printf("find get failed: id=%u\n", (unsigned)id);
        failed = 1;
    }
    if (zrpc_service_table_find_id("OrderService", "missing", &id) != ZRPC_ERR_NOTFOUND) {
        printf("missing method should not be found\n");
        failed = 1;
    }
    zrpc_reset_service_table();
    return failed;
}

static int test_message_view(void) {
    zrpc_message_view_t view;
    zrpc_payload_t pl;
    int failed = 0;
    const char borrowed[] = "borrowed";
    zrpc_message_view_init(&view);
    if (zrpc_message_view_set_string(&view, "hello") != ZRPC_OK || view.len != 5 ||
        memcmp(view.data, "hello", 5) != 0) {
        printf("set_string failed\n");
        failed = 1;
    }
    zrpc_message_view_release(&view);
    if (view.data != NULL || view.len != 0 || view.owned != NULL) {
        printf("release did not clear view\n");
        failed = 1;
    }
    if (zrpc_message_view_set_data(&view, "abc", 3) != ZRPC_OK) {
        printf("set_data failed\n");
        failed = 1;
    }
    zrpc_message_view_set_borrowed(&view, borrowed, sizeof(borrowed) - 1);
    if (view.owned != NULL) {
        printf("borrowed view should not own memory\n");
        failed = 1;
    }
    pl.data = view.data;
    pl.len = view.len;
    pl.encoding = 2;
    if (zrpc_message_view_set_payload(&view, &pl) != ZRPC_OK || view.encoding != 2 ||
        memcmp(view.data, borrowed, sizeof(borrowed) - 1) != 0) {
        printf("set_payload failed\n");
        failed = 1;
    }
    zrpc_message_view_release(&view);
    return failed;
}

static int echo_sync(const zrpc_request_t *req, zrpc_message_view_t *resp, void *user) {
    int *calls = (int *)user;
    if (calls) {
        (*calls)++;
    }
    (void)zrpc_message_view_set_data(resp, req->payload.data, req->payload.len);
    return 0;
}

static int test_stub_and_sync(void) {
    zrpc_node_config_t scfg;
    zrpc_node_config_t ccfg;
    zrpc_node_t *server = NULL;
    zrpc_node_t *client = NULL;
    zrpc_service_t *svc = NULL;
    zrpc_proxy_t *proxy = NULL;
    zrpc_stub_t *stub = NULL;
    zrpc_response_t response;
    zrpc_payload_t pl;
    uint16_t sport;
    uint16_t cport;
    int calls = 0;
    int failed = 0;
    int rc;

    sport = 42311;
    cport = 42312;

    memset(&scfg, 0, sizeof(scfg));
    scfg.name = "api_srv";
    scfg.transport = ZRPC_TRANSPORT_UDP;
    scfg.bind_host = "127.0.0.1";
    scfg.discovery_host = "127.0.0.1";
    scfg.discovery_port = sport;
    scfg.io_threads = 1;
    scfg.hello_interval_ms = 200;
    scfg.node_ttl_ms = 3000;
    scfg.service_table = &g_order_table;

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.name = "api_cli";
    ccfg.transport = ZRPC_TRANSPORT_UDP;
    ccfg.bind_host = "127.0.0.1";
    ccfg.discovery_host = "127.0.0.1";
    ccfg.discovery_port = cport;
    ccfg.io_threads = 1;
    ccfg.hello_interval_ms = 200;
    ccfg.node_ttl_ms = 3000;

    rc = zrpc_node_create(&scfg, &server);
    if (rc != ZRPC_OK) {
        printf("server create failed: %d\n", rc);
        return 1;
    }
    rc = zrpc_service_create(server, "OrderService", &svc);
    if (rc == ZRPC_OK) {
        rc = zrpc_service_add_sync_method(svc, "get", echo_sync, &calls);
    }
    if (rc != ZRPC_OK) {
        printf("service setup failed: %d\n", rc);
        zrpc_node_destroy(server);
        return 1;
    }
    rc = zrpc_node_create(&ccfg, &client);
    if (rc != ZRPC_OK) {
        printf("client create failed: %d\n", rc);
        zrpc_node_destroy(server);
        return 1;
    }
    (void)zrpc_node_add_discovery_peer(server, "127.0.0.1", cport);
    (void)zrpc_node_add_discovery_peer(client, "127.0.0.1", sport);

    if (zrpc_node_wait_for_service(client, "OrderService", 5000) != ZRPC_OK) {
        printf("service not discovered\n");
        failed = 1;
        goto cleanup;
    }

    /* stub 低层原语 + sync 便捷层（proxy 现在在 stub 之上）。 */
    rc = zrpc_stub_create(client, NULL, &stub);
    if (rc != ZRPC_OK || !stub) {
        printf("stub create failed: %d\n", rc);
        failed = 1;
        goto cleanup;
    }
    if (zrpc_stub_node(stub) != client) {
        printf("stub node mismatch\n");
        failed = 1;
    }

    rc = zrpc_proxy_create(client, NULL, &proxy);
    if (rc != ZRPC_OK) {
        printf("proxy create failed: %d\n", rc);
        failed = 1;
        goto cleanup;
    }
    memset(&response, 0, sizeof(response));
    pl.data = "ping";
    pl.len = 4;
    pl.encoding = 0;
    rc = zrpc_proxy_call_sync(proxy, "OrderService", "get", &pl, &response, 3000, NULL);
    if (rc != ZRPC_OK) {
        printf("call_sync failed: %d\n", rc);
        failed = 1;
    } else if (response.status != 0 || response.payload.len != 4 ||
               memcmp(response.payload.data, "ping", 4) != 0) {
        printf("call_sync bad response: status=%d len=%zu\n", response.status,
               response.payload.len);
        failed = 1;
    }
    zrpc_response_release(&response);

    /* proxy 的单向事件入口：同一 node 既可作为 service 也可作为 client。 */
    {
        int before = calls;
        uint64_t event_id = 0;
        rc = zrpc_proxy_call_event(proxy, "OrderService", "get", &pl, &event_id);
        if (rc != ZRPC_OK || event_id == 0) {
            printf("call_event failed: %d\n", rc);
            failed = 1;
        } else {
            ztk_sleep_ms(100);
            if (calls <= before) {
                printf("call_event was not delivered\n");
                failed = 1;
            }
        }
    }

cleanup:
    if (stub) {
        zrpc_stub_destroy(stub);
    }
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
    failed += test_service_table();
    failed += test_message_view();
    failed += test_stub_and_sync();
    if (failed) {
        printf("zri_api: FAIL\n");
        return 1;
    }
    printf("zri_api: OK\n");
    return 0;
}
