#ifndef RPC_LOG_H
#define RPC_LOG_H

#include <elog.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 首次调用时初始化 elog；线程安全，重复调用无副作用。 */
void rpc_log_init(void);

#ifdef __cplusplus
}
#endif

#ifndef RPC_LOG_TAG
#define RPC_LOG_TAG "rpc"
#endif

#define rpc_log_debug(...) \
    do {                   \
        rpc_log_init();    \
        elog_debug(RPC_LOG_TAG, __VA_ARGS__); \
    } while (0)

#define rpc_log_info(...) \
    do {                  \
        rpc_log_init();   \
        elog_info(RPC_LOG_TAG, __VA_ARGS__); \
    } while (0)

#define rpc_log_warn(...) \
    do {                  \
        rpc_log_init();   \
        elog_warn(RPC_LOG_TAG, __VA_ARGS__); \
    } while (0)

#define rpc_log_error(...) \
    do {                   \
        rpc_log_init();    \
        elog_error(RPC_LOG_TAG, __VA_ARGS__); \
    } while (0)

#endif /* RPC_LOG_H */
