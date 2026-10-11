/*
 * 01 你好，Node：只演示节点生命周期与配置。
 *
 * 与 fastcall-c 的 01_hello_node 对应：创建 -> 查询 -> 销毁。
 * 这里用 STATIC 模式，不启动服务发现，离线即可运行。
 */
#include <zrpc/zrpc.h>

#include <stdio.h>
#include <string.h>

int main(void) {
    zrpc_node_config_t cfg;
    zrpc_node_t *node = NULL;
    int rc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "HelloNode";
    cfg.transport = ZRPC_TRANSPORT_UDP;
    cfg.bind_host = "127.0.0.1";
    cfg.mode = ZRPC_MODE_STATIC;
    cfg.io_threads = 1;

    rc = zrpc_node_create(&cfg, &node);
    if (rc != ZRPC_OK) {
        fprintf(stderr, "zrpc_node_create failed: %d\n", rc);
        return 1;
    }
    printf("node=%s transport=%s data_port=%u bindings=%zu\n", zrpc_node_name(node),
           zrpc_node_transport(node) == ZRPC_TRANSPORT_TCP ? "tcp" : "udp",
           (unsigned)zrpc_node_data_port(node), zrpc_node_binding_count(node));
    zrpc_node_destroy(node);
    return 0;
}
