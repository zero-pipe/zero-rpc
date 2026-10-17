/*
 * 04 Service Table：两端共享的声明契约。对应 fastcall-c 的 fc_service_table。
 *
 * 在真实项目里，ZRPC_SERVICE_DEFINE 放在客户端与服务端共享的头文件里；这里为
 * 单文件示例直接内联。install 之后即可按名字查到数字 ID。
 */
#include <zrpc/zrpc_extensions.h>

#include <stdio.h>

ZRPC_SERVICE_DEFINE(order_table, 1u, "OrderService",
                    ZRPC_SERVICE_METHOD(1u, "get"),
                    ZRPC_SERVICE_METHOD(2u, "put"));

int main(void) {
    uint16_t id = 0;
    int rc;

    rc = zrpc_install_service_table(&order_table);
    if (rc != ZRPC_OK) {
        fprintf(stderr, "install table failed: %d\n", rc);
        return 1;
    }
    if (zrpc_service_table_find_id("OrderService", "get", &id) == ZRPC_OK) {
        printf("OrderService.get -> id=%u\n", (unsigned)id);
    }
    if (zrpc_service_table_find_id("OrderService", "put", &id) == ZRPC_OK) {
        printf("OrderService.put -> id=%u\n", (unsigned)id);
    }
    zrpc_reset_service_table();
    return 0;
}
