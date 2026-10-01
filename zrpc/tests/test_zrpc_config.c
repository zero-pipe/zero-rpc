#include <zrpc/zrpc.h>

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail = 1;                                            \
        }                                                          \
    } while (0)

static void test_parse(void) {
    const char *json =
        "{"
        "\"node\":{\"name\":\"n1\",\"transport\":\"tcp\",\"data_port\":9000,"
        "\"io_threads\":2,\"mode\":\"static\",\"frag_bytes\":1024,\"max_msg_bytes\":4194304},"
        "\"discovery\":{\"host\":\"127.0.0.1\",\"port\":41953,\"hello_interval_ms\":500,"
        "\"node_ttl_ms\":9000,"
        "\"peers\":[{\"ip\":\"10.0.0.1\",\"port\":41953}]},"
        "\"routes\":[{\"service\":\"echo\",\"ip\":\"10.0.0.2\",\"port\":9001}]"
        "}";
    zrpc_config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    CHECK(zrpc_config_load(json, &cfg) == ZRPC_OK);
    CHECK(strcmp(cfg.name, "n1") == 0);
    CHECK(cfg.node.transport == ZRPC_TRANSPORT_TCP);
    CHECK(cfg.node.mode == ZRPC_MODE_STATIC);
    CHECK(cfg.node.data_port == 9000);
    CHECK(cfg.node.io_threads == 2);
    CHECK(cfg.node.frag_bytes == 1024);
    CHECK(cfg.node.max_msg_bytes == 4194304);
    CHECK(strcmp(cfg.discovery_host, "127.0.0.1") == 0);
    CHECK(cfg.node.discovery_port == 41953);
    CHECK(cfg.node.hello_interval_ms == 500);
    CHECK(cfg.peer_count == 1);
    CHECK(strcmp(cfg.peers[0].ip, "10.0.0.1") == 0);
    CHECK(cfg.peers[0].port == 41953);
    CHECK(cfg.route_count == 1);
    CHECK(strcmp(cfg.routes[0].service, "echo") == 0);
    CHECK(strcmp(cfg.routes[0].ip, "10.0.0.2") == 0);
    CHECK(cfg.routes[0].port == 9001);
    /* 未显式给出时，路由传输种类继承 node.transport。 */
    CHECK(cfg.routes[0].transport == ZRPC_TRANSPORT_TCP);
}

static void test_defaults(void) {
    zrpc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(zrpc_config_load("{\"node\":{}}", &cfg) == ZRPC_OK);
    CHECK(cfg.node.transport == ZRPC_TRANSPORT_UDP);
    CHECK(cfg.node.mode == ZRPC_MODE_MESH);
    CHECK(strcmp(cfg.name, "zrpc") == 0);
    CHECK(strcmp(cfg.bind_host, "0.0.0.0") == 0);
    CHECK(strcmp(cfg.discovery_host, "255.255.255.255") == 0);
}

static void test_bad_json(void) {
    zrpc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(zrpc_config_load("{ not json", &cfg) == ZRPC_ERR_INVALID);
    CHECK(zrpc_config_load(NULL, &cfg) == ZRPC_ERR_INVALID);
}

static void test_endpoint_model(void) {
    zrpc_endpoint_t endpoint;
    char text[128];
    CHECK(zrpc_endpoint_parse("tcp://127.0.0.1:8080", &endpoint) == ZRPC_OK);
    CHECK(endpoint.transport == ZRPC_TRANSPORT_TCP);
    CHECK(endpoint.port == 8080);
    CHECK(zrpc_endpoint_format(&endpoint, text, sizeof(text)) == ZRPC_OK);
    CHECK(strcmp(text, "tcp://127.0.0.1:8080") == 0);
    CHECK(zrpc_endpoint_parse("dds://127.0.0.1:7400", &endpoint) == ZRPC_OK);
    CHECK(endpoint.transport == ZRPC_TRANSPORT_CUSTOM);
}

static void test_binding_config(void) {
    const char *json =
        "{\"node\":{\"name\":\"dual\",\"mode\":\"static\"},"
        "\"transport\":{\"bindings\":["
        "{\"name\":\"tcp\",\"endpoint\":\"tcp://127.0.0.1:8080\"},"
        "{\"name\":\"udp\",\"endpoint\":\"udp://127.0.0.1:8081\"}]},"
        "\"routes\":[{\"service\":\"order\","
        "\"endpoint\":\"tcp://127.0.0.1:9000\"}]}";
    zrpc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(zrpc_config_load(json, &cfg) == ZRPC_OK);
    CHECK(cfg.binding_count == 2);
    CHECK(strcmp(cfg.binding_configs[0].name, "tcp") == 0);
    CHECK(strcmp(cfg.binding_configs[1].endpoint, "udp://127.0.0.1:8081") == 0);
    CHECK(cfg.route_count == 1);
    CHECK(strcmp(cfg.routes[0].endpoint, "tcp://127.0.0.1:9000") == 0);
}

int main(void) {
    test_parse();
    test_defaults();
    test_bad_json();
    test_endpoint_model();
    test_binding_config();
    if (g_fail) {
        printf("zrpc_config: FAIL\n");
        return 1;
    }
    printf("zrpc_config: OK\n");
    return 0;
}
