/*
 * 04 Service Table：两端共享的声明契约。对应 fastcall-c 的 fc_service_table。
 *
 * 在真实项目里，SERVICE_DEFINE 放在客户端与服务端共享的头文件里；这里为
 * 单文件示例直接内联。install 之后即可按名字查到数字 ID。
 */
#include <extensions.h>

#define RPC_LOG_TAG "example"
#include <log.h>

SERVICE_DEFINE(
    order_table, 1u, "OrderService", SERVICE_METHOD(1u, "get"), SERVICE_METHOD(2u, "put"));

int main(void)
{
    uint16_t id = 0;
    int rc;

    rc = install_service_table(&order_table);
    if (rc != OK) {
        rpc_log_error("install table failed: %d", rc);
        return 1;
    }
    if (service_table_find_id("OrderService", "get", &id) == OK) {
        rpc_log_info("OrderService.get -> id=%u", (unsigned)id);
    }
    if (service_table_find_id("OrderService", "put", &id) == OK) {
        rpc_log_info("OrderService.put -> id=%u", (unsigned)id);
    }
    reset_service_table();
    return 0;
}
