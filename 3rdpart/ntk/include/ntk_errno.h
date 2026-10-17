#ifndef NTK_ERRNO_H
#define NTK_ERRNO_H

#include "ntk_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ntk_err {
    NTK_OK = 0,
    NTK_ERR_NOMEM = -1,
    NTK_ERR_INVALID = -2,
    NTK_ERR_IO = -3,
    NTK_ERR_TIMEOUT = -4,
    NTK_ERR_AGAIN = -5,
    NTK_ERR_NOT_IMPL = -6,
    NTK_ERR_PLATFORM = -7,
    NTK_ERR_STATE = -8,
    NTK_ERR_BUFFER_TOO_SMALL = -9
} ntk_err_t;

/** 线程局部或全局最后一次错误的补充描述（只读，勿 free） */
NTK_API const char *ntk_strerror(ntk_err_t err);
NTK_API void ntk_set_last_error(const char *msg);
NTK_API const char *ntk_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* NTK_ERRNO_H */
