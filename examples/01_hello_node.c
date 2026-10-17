/*
 * 01 你好，Node：只演示节点生命周期与配置。
 *
 * 与 fastcall-c 的 01_hello_node 对应：创建 -> 查询 -> 销毁。
 * 这里用 STATIC 模式，不启动服务发现，离线即可运行。
 */
#include <runtime.h>

#include <stdio.h>
#include <string.h>

int main(void) {
    node_config_t cfg;
    node_t *node = NULL;
    int rc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "HelloNode";
    cfg.transport = TRANSPORT_UDP;
    cfg.bind_host = "127.0.0.1";
    cfg.mode = MODE_STATIC;
    cfg.io_threads = 1;

    rc = node_create(&cfg, &node);
    if (rc != OK) {
        fprintf(stderr, "node_create failed: %d\n", rc);
        return 1;
    }
    printf("node=%s transport=%s data_port=%u bindings=%zu\n", node_name(node),
           node_transport(node) == TRANSPORT_TCP ? "tcp" : "udp",
           (unsigned)node_data_port(node), node_binding_count(node));
    node_destroy(node);
    return 0;
}
