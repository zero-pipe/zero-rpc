#ifndef ZTK_ERRNO_H
#define ZTK_ERRNO_H

#include "ztk_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ztk_err {
    ZTK_OK = 0,
    ZTK_ERR_NOMEM = -1,
    ZTK_ERR_INVALID = -2,
    ZTK_ERR_IO = -3,
    ZTK_ERR_TIMEOUT = -4,
    ZTK_ERR_AGAIN = -5,
    ZTK_ERR_NOT_IMPL = -6,
    ZTK_ERR_PLATFORM = -7,
    ZTK_ERR_STATE = -8,
    ZTK_ERR_BUFFER_TOO_SMALL = -9
} ztk_err_t;

/** 线程局部或全局最后一次错误的补充描述（只读，勿 free） */
ZTK_API const char *ztk_strerror(ztk_err_t err);
ZTK_API void ztk_set_last_error(const char *msg);
ZTK_API const char *ztk_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* ZTK_ERRNO_H */
