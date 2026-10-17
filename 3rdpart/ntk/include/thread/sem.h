#ifndef NTK_THREAD_SEM_H
#define NTK_THREAD_SEM_H

#include "../ntk_export.h"
#include "../ntk_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ntk_sem ntk_sem;

NTK_API ntk_sem *ntk_sem_create(unsigned int initial);
NTK_API void ntk_sem_destroy(ntk_sem *s);
NTK_API ntk_err_t ntk_sem_wait(ntk_sem *s);
/** @return NTK_OK 或 NTK_ERR_TIMEOUT */
NTK_API ntk_err_t ntk_sem_timedwait(ntk_sem *s, unsigned int timeout_ms);
NTK_API ntk_err_t ntk_sem_post(ntk_sem *s, unsigned int count);

#ifdef __cplusplus
}
#endif

#endif /* NTK_THREAD_SEM_H */
