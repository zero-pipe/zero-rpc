#ifndef NTK_UTIL_LOG_H
#define NTK_UTIL_LOG_H

#include "../ntk_export.h"
#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ntk_log_level {
    NTK_LOG_TRACE = 0,
    NTK_LOG_DEBUG,
    NTK_LOG_INFO,
    NTK_LOG_WARN,
    NTK_LOG_ERROR
} ntk_log_level_t;

NTK_API void ntk_log_set_level(ntk_log_level_t level);
NTK_API int  ntk_log_open_file(const char *path);
NTK_API void ntk_log_close_file(void);
/** 关闭并重新打开当前日志文件（供 SIGHUP / logrotate 使用）。未打开文件时无操作。 */
NTK_API void ntk_log_reopen_file(void);
NTK_API void ntk_log_write(ntk_log_level_t level, const char *file, int line, const char *fmt, ...);
NTK_API void ntk_log_writev(ntk_log_level_t level, const char *file, int line, const char *fmt, va_list ap);

/**
 * 配置日志文件按大小自动轮转（需在 ntk_log_open_file 之后调用）。
 * @param max_bytes  单个日志文件最大字节数；0 = 不轮转。
 * @param keep_count 保留旧文件数量（.log.1 … .log.N）；建议 3-10。
 */
NTK_API void ntk_log_set_rotate(size_t max_bytes, int keep_count);

#define NTK_LOG(lvl, fmt, ...) \
    ntk_log_write((lvl), __FILE__, __LINE__, (fmt), ##__VA_ARGS__)

#define ntk_trace(fmt, ...) NTK_LOG(NTK_LOG_TRACE, fmt, ##__VA_ARGS__)
#define ntk_debug(fmt, ...) NTK_LOG(NTK_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define ntk_info(fmt, ...)  NTK_LOG(NTK_LOG_INFO, fmt, ##__VA_ARGS__)
#define ntk_warn(fmt, ...)  NTK_LOG(NTK_LOG_WARN, fmt, ##__VA_ARGS__)
#define ntk_error(fmt, ...) NTK_LOG(NTK_LOG_ERROR, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* NTK_UTIL_LOG_H */
