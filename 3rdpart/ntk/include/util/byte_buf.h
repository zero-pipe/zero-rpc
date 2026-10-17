#ifndef NTK_UTIL_BYTE_BUF_H
#define NTK_UTIL_BYTE_BUF_H

/**
 * 单所有者可扩容字节缓冲：协议解析等 append/clear 场景。
 * 与 ntk_buf（引用计数共享载荷）无关。
 */
#include "../ntk_export.h"
#include "../ntk_errno.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ntk_byte_buf ntk_byte_buf;

NTK_API ntk_byte_buf *ntk_byte_buf_create(size_t initial_capacity);
NTK_API void ntk_byte_buf_destroy(ntk_byte_buf *b);

NTK_API const char *ntk_byte_buf_data(const ntk_byte_buf *b);
NTK_API size_t ntk_byte_buf_size(const ntk_byte_buf *b);
NTK_API size_t ntk_byte_buf_capacity(const ntk_byte_buf *b);

NTK_API ntk_err_t ntk_byte_buf_reserve(ntk_byte_buf *b, size_t capacity);
NTK_API ntk_err_t ntk_byte_buf_append(ntk_byte_buf *b, const void *data, size_t len);
NTK_API ntk_err_t ntk_byte_buf_clear(ntk_byte_buf *b);
/** 丢弃缓冲区前 n 字节 */
NTK_API ntk_err_t ntk_byte_buf_erase_front(ntk_byte_buf *b, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* NTK_UTIL_BYTE_BUF_H */
