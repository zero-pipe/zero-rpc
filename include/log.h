#ifndef NB_LOG_H
#define NB_LOG_H

#include <elog.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 首次调用时初始化 elog；线程安全，重复调用无副作用。 */
void nb_log_init(void);

/*
 * 运行期开关：enabled=0 时所有 nb_log_* 宏短路，不做任何格式化/输出。
 * level 见 node_log_level 定义（与 elog 级别一致：0=assert .. 5=verbose）。
 * nb_node_create 会按 log 配置调用一次。
 */
void nb_log_configure(int enabled, uint8_t level);

/* 当前是否启用日志；首次调用会完成 elog 初始化。 */
int nb_log_enabled(void);

#ifdef __cplusplus
}
#endif

#ifndef NB_LOG_TAG
#define NB_LOG_TAG "rpc"
#endif

#define nb_log_debug(...)                            \
    do {                                              \
        if (nb_log_enabled()) {                      \
            elog_debug(NB_LOG_TAG, __VA_ARGS__);     \
        }                                             \
    } while (0)

#define nb_log_info(...)                            \
    do {                                             \
        if (nb_log_enabled()) {                     \
            elog_info(NB_LOG_TAG, __VA_ARGS__);     \
        }                                            \
    } while (0)

#define nb_log_warn(...)                            \
    do {                                             \
        if (nb_log_enabled()) {                     \
            elog_warn(NB_LOG_TAG, __VA_ARGS__);     \
        }                                            \
    } while (0)

#define nb_log_error(...)                           \
    do {                                             \
        if (nb_log_enabled()) {                     \
            elog_error(NB_LOG_TAG, __VA_ARGS__);    \
        }                                            \
    } while (0)

#endif /* NB_LOG_H */
