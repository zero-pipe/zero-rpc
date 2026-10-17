#ifndef SERVICE_TABLE_H
#define SERVICE_TABLE_H

/*
 * 服务/方法声明表（共享契约）。
 *
 * 对齐 fastcall-c 的 fc_service_table：把 service / method 及其数字 ID 写进一个
 * 两端共享的头文件，客户端与服务端都 install 同一张表，从而对命名与编号达成
 * 一致。数字 ID 目前用于声明校验与元数据（路由仍以 "service.method" 字符串为准）。
 *
 * 典型用法（放在两端共享的头文件里）：
 *
 *     SERVICE_DEFINE(order_table, 1u, "OrderService",
 *         SERVICE_METHOD(1u, "get"),
 *         SERVICE_METHOD(2u, "put"));
 *
 * 然后把 &order_table 填进 node_config_t::service_table，或在创建节点前调用
 * install_service_table(&order_table)。
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 声明结构 ---- */
typedef struct method_descriptor {
    uint16_t id;
    const char *name;
} method_descriptor_t;

typedef struct service_descriptor {
    uint16_t id;
    const char *name;
    const method_descriptor_t *methods;
    size_t method_count;
} service_descriptor_t;

typedef struct service_table {
    const service_descriptor_t *services;
    size_t service_count;
} service_table_t;

/* ---- 安装与查询 ---- */
/*
 * 安装一张声明表。可在创建节点前多次安装（多张表并存）；重复安装同一声明是
 * 幂等的。同名 service/method 但 ID 不同会被拒绝并返回 ERR_INVALID。
 * 表本身按指针借用，必须具有静态生命周期（用 SERVICE_DEFINE 生成即可）。
 */
int install_service_table(const service_table_t *table);

/* 清空所有已安装声明。主要用于测试。 */
void reset_service_table(void);

/* 当前是否安装过任何声明表（热路径快速判断）。 */
int service_table_active(void);

/* 查询 service.method 的数字 ID；找到返回 OK。任何参数为空均返回 ERR_INVALID。 */
int service_table_find_id(const char *service, const char *method, uint16_t *out_id);

/* ---- 声明宏 ---- */
/* 声明一行方法，只在 SERVICE_DEFINE() 内使用。 */
#define SERVICE_METHOD(id, name) {(uint16_t)(id), (name)}

/* 声明单服务表；展开出 <table>_methods / <table>_service / <table> 三个静态对象。 */
#define SERVICE_DEFINE(table, service_id, service_name, ...)                         \
    static const method_descriptor_t table##_methods[] = {__VA_ARGS__};              \
    static const service_descriptor_t table##_service = {                            \
        (uint16_t)(service_id), (service_name), table##_methods,                          \
        sizeof(table##_methods) / sizeof(table##_methods[0])};                            \
    static const service_table_t table = {&table##_service, 1u}

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_TABLE_H */
