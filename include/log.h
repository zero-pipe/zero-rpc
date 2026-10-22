#ifndef RPC_LOG_H
#define RPC_LOG_H

#include <elog.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 首次调用时初始化 elog；线程安全，重复调用无副作用。 */
void rpc_log_init(void);

/*
 * 运行期开关：enabled=0 时所有 rpc_log_* 宏短路，不做任何格式化/输出。
 * level 见 node_log_level 定义（与 elog 级别一致：0=assert .. 5=verbose）。
 * node_create 会按 log 配置调用一次。
 */
void rpc_log_configure(int enabled, uint8_t level);

/* 当前是否启用日志；首次调用会完成 elog 初始化。 */
int rpc_log_enabled(void);

#ifdef __cplusplus
}
#endif

#ifndef RPC_LOG_TAG
#define RPC_LOG_TAG "rpc"
#endif

#define rpc_log_debug(...)                            \
    do {                                              \
        if (rpc_log_enabled()) {                      \
            elog_debug(RPC_LOG_TAG, __VA_ARGS__);     \
        }                                             \
    } while (0)

#define rpc_log_info(...)                            \
    do {                                             \
        if (rpc_log_enabled()) {                     \
            elog_info(RPC_LOG_TAG, __VA_ARGS__);     \
        }                                            \
    } while (0)

#define rpc_log_warn(...)                            \
    do {                                             \
        if (rpc_log_enabled()) {                     \
            elog_warn(RPC_LOG_TAG, __VA_ARGS__);     \
        }                                            \
    } while (0)

#define rpc_log_error(...)                           \
    do {                                             \
        if (rpc_log_enabled()) {                     \
            elog_error(RPC_LOG_TAG, __VA_ARGS__);    \
        }                                            \
    } while (0)

#endif /* RPC_LOG_H */
