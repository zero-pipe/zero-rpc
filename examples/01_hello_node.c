/*
 * 01 你好，Node：只演示节点生命周期与配置。
 *
 * 与 fastcall-c 的 01_hello_node 对应：创建 -> 查询 -> 销毁。
 * 这里用 STATIC 模式，不启动服务发现，离线即可运行。
 */
#include <runtime.h>

#define NB_LOG_TAG "example"
#include <log.h>

#include <string.h>

int main(void)
{
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    int rc;

    nb_node_config_default(&cfg);
    cfg.name = "HelloNode";
    cfg.transport.transport = NB_TRANSPORT_UDP;
    cfg.transport.bind_host = "127.0.0.1";
    cfg.mode = NB_MODE_STATIC;
    cfg.runtime.io_threads = 1;

    rc = nb_node_create(&cfg, &node);
    if (rc != NB_OK) {
        nb_log_error("nb_node_create failed: %d", rc);
        return 1;
    }
    nb_log_info("node=%s transport=%s data_port=%u bindings=%zu",
                 nb_node_name(node),
                 nb_node_transport(node) == NB_TRANSPORT_TCP ? "tcp" : "udp",
                 (unsigned)nb_node_data_port(node),
                 nb_node_binding_count(node));
    nb_node_destroy(node);
    return 0;
}
