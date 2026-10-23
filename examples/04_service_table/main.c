/*
 * 04 Service Table：两端共享的声明契约。对应 fastcall-c 的 fc_service_table。
 *
 * 在真实项目里，NB_SERVICE_DEFINE 放在客户端与服务端共享的头文件里；这里为
 * 单文件示例直接内联。install 之后即可按名字查到数字 ID。
 */
#include <extensions.h>

#define NB_LOG_TAG "example"
#include <log.h>

NB_SERVICE_DEFINE(
    order_table, 1u, "OrderService", NB_SERVICE_METHOD(1u, "get"), NB_SERVICE_METHOD(2u, "put"));

int main(void)
{
    uint16_t id = 0;
    int rc;

    rc = nb_install_service_table(&order_table);
    if (rc != NB_OK) {
        nb_log_error("install table failed: %d", rc);
        return 1;
    }
    if (nb_service_table_find_id("OrderService", "get", &id) == NB_OK) {
        nb_log_info("OrderService.get -> id=%u", (unsigned)id);
    }
    if (nb_service_table_find_id("OrderService", "put", &id) == NB_OK) {
        nb_log_info("OrderService.put -> id=%u", (unsigned)id);
    }
    nb_reset_service_table();
    return 0;
}
