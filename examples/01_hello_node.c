/*
 * 01 你好，Node：只演示节点生命周期与配置。
 *
 * 与 fastcall-c 的 01_hello_node 对应：创建 -> 查询 -> 销毁。
 * 这里用 STATIC 模式，不启动服务发现，离线即可运行。
 */
#include <runtime.h>

#define RPC_LOG_TAG "example"
#include <log.h>

#include <string.h>

int main(void)
{
    node_config_t cfg;
    node_t *node = NULL;
    int rc;

    node_config_default(&cfg);
    cfg.name = "HelloNode";
    cfg.transport.transport = TRANSPORT_UDP;
    cfg.transport.bind_host = "127.0.0.1";
    cfg.mode = MODE_STATIC;
    cfg.runtime.io_threads = 1;

    rc = node_create(&cfg, &node);
    if (rc != OK) {
        rpc_log_error("node_create failed: %d", rc);
        return 1;
    }
    rpc_log_info("node=%s transport=%s data_port=%u bindings=%zu",
                 node_name(node),
                 node_transport(node) == TRANSPORT_TCP ? "tcp" : "udp",
                 (unsigned)node_data_port(node),
                 node_binding_count(node));
    node_destroy(node);
    return 0;
}
