/*
 * 03 Proxy：客户端代理生命周期。对应 fastcall-c 的 03_proxy：
 * 只创建 node + proxy（内部构建在 stub 之上），展示分层与释放顺序。
 */
#include <zrpc/zrpc_client.h>

#include <stdio.h>
#include <string.h>

int main(void) {
    zrpc_node_config_t cfg;
    zrpc_proxy_options_t options;
    zrpc_node_t *node = NULL;
    zrpc_proxy_t *proxy = NULL;
    int rc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "ProxyExample";
    cfg.transport = ZRPC_TRANSPORT_TCP;
    cfg.bind_host = "127.0.0.1";
    cfg.mode = ZRPC_MODE_STATIC;
    cfg.io_threads = 1;
    rc = zrpc_node_create(&cfg, &node);
    if (rc != ZRPC_OK) {
        fprintf(stderr, "node create failed: %d\n", rc);
        return 1;
    }

    memset(&options, 0, sizeof(options));
    options.payload_encoding = 1; /* 默认写入请求的编码标识 */
    rc = zrpc_proxy_create(node, &options, &proxy);
    if (rc != ZRPC_OK) {
        fprintf(stderr, "proxy create failed: %d\n", rc);
        zrpc_node_destroy(node);
        return 1;
    }
    printf("created node=%s and proxy\n", zrpc_node_name(node));

    zrpc_proxy_destroy(proxy);
    zrpc_node_destroy(node);
    return 0;
}
