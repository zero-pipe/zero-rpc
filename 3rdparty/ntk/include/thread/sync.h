#ifndef NTK_THREAD_SYNC_H
#define NTK_THREAD_SYNC_H

#include "../ntk_export.h"
#include "../ntk_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ntk_mutex ntk_mutex;

enum {
    NTK_MUTEX_NORMAL = 0,
    NTK_MUTEX_RECURSIVE = 1
};

NTK_API ntk_mutex *ntk_mutex_create(int flags);
NTK_API void ntk_mutex_destroy(ntk_mutex *m);
NTK_API ntk_err_t ntk_mutex_lock(ntk_mutex *m);
NTK_API ntk_err_t ntk_mutex_unlock(ntk_mutex *m);
NTK_API ntk_err_t ntk_mutex_trylock(ntk_mutex *m);

#ifdef __cplusplus
}
#endif

#endif /* NTK_THREAD_SYNC_H */
