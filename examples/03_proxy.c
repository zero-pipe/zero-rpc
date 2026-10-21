/*
 * 03 Proxy：客户端代理生命周期。对应 fastcall-c 的 03_proxy：
 * 只创建 node + proxy（内部构建在 stub 之上），展示分层与释放顺序。
 */
#include <client.h>

#define RPC_LOG_TAG "example"
#include <log.h>

#include <string.h>

int main(void)
{
    node_config_t cfg;
    proxy_options_t options;
    node_t *node = NULL;
    proxy_t *proxy = NULL;
    int rc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "ProxyExample";
    cfg.transport = TRANSPORT_TCP;
    cfg.bind_host = "127.0.0.1";
    cfg.mode = MODE_STATIC;
    cfg.io_threads = 1;
    rc = node_create(&cfg, &node);
    if (rc != OK) {
        rpc_log_error("node create failed: %d", rc);
        return 1;
    }

    memset(&options, 0, sizeof(options));
    options.payload_encoding = 1; /* 默认写入请求的编码标识 */
    rc = proxy_create(node, &options, &proxy);
    if (rc != OK) {
        rpc_log_error("proxy create failed: %d", rc);
        node_destroy(node);
        return 1;
    }
    rpc_log_info("created node=%s and proxy", node_name(node));

    proxy_destroy(proxy);
    node_destroy(node);
    return 0;
}
