#ifndef ZTK_THREAD_SYNC_H
#define ZTK_THREAD_SYNC_H

#include "../ztk_export.h"
#include "../ztk_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ztk_mutex ztk_mutex;

enum {
    ZTK_MUTEX_NORMAL = 0,
    ZTK_MUTEX_RECURSIVE = 1
};

ZTK_API ztk_mutex *ztk_mutex_create(int flags);
ZTK_API void ztk_mutex_destroy(ztk_mutex *m);
ZTK_API ztk_err_t ztk_mutex_lock(ztk_mutex *m);
ZTK_API ztk_err_t ztk_mutex_unlock(ztk_mutex *m);
ZTK_API ztk_err_t ztk_mutex_trylock(ztk_mutex *m);

#ifdef __cplusplus
}
#endif

#endif /* ZTK_THREAD_SYNC_H */
