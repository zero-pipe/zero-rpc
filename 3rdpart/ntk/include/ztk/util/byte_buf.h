#ifndef ZTK_UTIL_BYTE_BUF_H
#define ZTK_UTIL_BYTE_BUF_H

/**
 * 单所有者可扩容字节缓冲：协议解析等 append/clear 场景。
 * 与 ztk_buf（引用计数共享载荷）无关。
 */
#include "../ztk_export.h"
#include "../ztk_errno.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ztk_byte_buf ztk_byte_buf;

ZTK_API ztk_byte_buf *ztk_byte_buf_create(size_t initial_capacity);
ZTK_API void ztk_byte_buf_destroy(ztk_byte_buf *b);

ZTK_API const char *ztk_byte_buf_data(const ztk_byte_buf *b);
ZTK_API size_t ztk_byte_buf_size(const ztk_byte_buf *b);
ZTK_API size_t ztk_byte_buf_capacity(const ztk_byte_buf *b);

ZTK_API ztk_err_t ztk_byte_buf_reserve(ztk_byte_buf *b, size_t capacity);
ZTK_API ztk_err_t ztk_byte_buf_append(ztk_byte_buf *b, const void *data, size_t len);
ZTK_API ztk_err_t ztk_byte_buf_clear(ztk_byte_buf *b);
/** 丢弃缓冲区前 n 字节 */
ZTK_API ztk_err_t ztk_byte_buf_erase_front(ztk_byte_buf *b, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* ZTK_UTIL_BYTE_BUF_H */
