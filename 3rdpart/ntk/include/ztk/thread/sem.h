#ifndef ZTK_THREAD_SEM_H
#define ZTK_THREAD_SEM_H

#include "../ztk_export.h"
#include "../ztk_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ztk_sem ztk_sem;

ZTK_API ztk_sem *ztk_sem_create(unsigned int initial);
ZTK_API void ztk_sem_destroy(ztk_sem *s);
ZTK_API ztk_err_t ztk_sem_wait(ztk_sem *s);
/** @return ZTK_OK 或 ZTK_ERR_TIMEOUT */
ZTK_API ztk_err_t ztk_sem_timedwait(ztk_sem *s, unsigned int timeout_ms);
ZTK_API ztk_err_t ztk_sem_post(ztk_sem *s, unsigned int count);

#ifdef __cplusplus
}
#endif

#endif /* ZTK_THREAD_SEM_H */
