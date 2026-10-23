/*
 * 03 Proxy：客户端代理生命周期。对应 fastcall-c 的 03_proxy：
 * 只创建 node + proxy（内部构建在 stub 之上），展示分层与释放顺序。
 */
#include <client.h>

#define NB_LOG_TAG "example"
#include <log.h>

#include <string.h>

int main(void)
{
    nb_node_config_t cfg;
    nb_proxy_options_t options;
    nb_node_t *node = NULL;
    nb_proxy_t *proxy = NULL;
    int rc;

    nb_node_config_default(&cfg);
    cfg.name = "ProxyExample";
    cfg.transport.transport = NB_TRANSPORT_TCP;
    cfg.transport.bind_host = "127.0.0.1";
    cfg.mode = NB_MODE_STATIC;
    cfg.runtime.io_threads = 1;
    rc = nb_node_create(&cfg, &node);
    if (rc != NB_OK) {
        nb_log_error("node create failed: %d", rc);
        return 1;
    }

    memset(&options, 0, sizeof(options));
    options.payload_encoding = 1; /* 默认写入请求的编码标识 */
    rc = nb_proxy_create(node, &options, &proxy);
    if (rc != NB_OK) {
        nb_log_error("proxy create failed: %d", rc);
        nb_node_destroy(node);
        return 1;
    }
    nb_log_info("created node=%s and proxy", nb_node_name(node));

    nb_proxy_destroy(proxy);
    nb_node_destroy(node);
    return 0;
}
