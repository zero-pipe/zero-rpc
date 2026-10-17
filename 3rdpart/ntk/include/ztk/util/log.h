#ifndef ZTK_UTIL_LOG_H
#define ZTK_UTIL_LOG_H

#include "../ztk_export.h"
#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ztk_log_level {
    ZTK_LOG_TRACE = 0,
    ZTK_LOG_DEBUG,
    ZTK_LOG_INFO,
    ZTK_LOG_WARN,
    ZTK_LOG_ERROR
} ztk_log_level_t;

ZTK_API void ztk_log_set_level(ztk_log_level_t level);
ZTK_API int  ztk_log_open_file(const char *path);
ZTK_API void ztk_log_close_file(void);
/** 关闭并重新打开当前日志文件（供 SIGHUP / logrotate 使用）。未打开文件时无操作。 */
ZTK_API void ztk_log_reopen_file(void);
ZTK_API void ztk_log_write(ztk_log_level_t level, const char *file, int line, const char *fmt, ...);
ZTK_API void ztk_log_writev(ztk_log_level_t level, const char *file, int line, const char *fmt, va_list ap);

/**
 * 配置日志文件按大小自动轮转（需在 ztk_log_open_file 之后调用）。
 * @param max_bytes  单个日志文件最大字节数；0 = 不轮转。
 * @param keep_count 保留旧文件数量（.log.1 … .log.N）；建议 3-10。
 */
ZTK_API void ztk_log_set_rotate(size_t max_bytes, int keep_count);

#define ZTK_LOG(lvl, fmt, ...) \
    ztk_log_write((lvl), __FILE__, __LINE__, (fmt), ##__VA_ARGS__)

#define ztk_trace(fmt, ...) ZTK_LOG(ZTK_LOG_TRACE, fmt, ##__VA_ARGS__)
#define ztk_debug(fmt, ...) ZTK_LOG(ZTK_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define ztk_info(fmt, ...)  ZTK_LOG(ZTK_LOG_INFO, fmt, ##__VA_ARGS__)
#define ztk_warn(fmt, ...)  ZTK_LOG(ZTK_LOG_WARN, fmt, ##__VA_ARGS__)
#define ztk_error(fmt, ...) ZTK_LOG(ZTK_LOG_ERROR, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* ZTK_UTIL_LOG_H */
