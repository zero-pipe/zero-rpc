#ifndef ZTK_UTIL_MPSC_H
#define ZTK_UTIL_MPSC_H

#include "../ztk_export.h"
#include "../ztk_errno.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ztk_mpsc_queue ztk_mpsc_queue;

/** cap_pow2 须为 2 的幂且 ≥ 2 */
ZTK_API ztk_mpsc_queue *ztk_mpsc_create(size_t cap_pow2);
ZTK_API void ztk_mpsc_destroy(ztk_mpsc_queue *q);

/** 多生产者 push；队列满返回 ZTK_ERR_AGAIN */
ZTK_API ztk_err_t ztk_mpsc_push(ztk_mpsc_queue *q, void *item);
/** 单消费者 pop；空返回 NULL */
ZTK_API void *ztk_mpsc_pop(ztk_mpsc_queue *q);

#ifdef __cplusplus
}
#endif

#endif /* ZTK_UTIL_MPSC_H */
