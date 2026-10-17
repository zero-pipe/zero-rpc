#ifndef NTK_UTIL_MPSC_H
#define NTK_UTIL_MPSC_H

#include "../ntk_export.h"
#include "../ntk_errno.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ntk_mpsc_queue ntk_mpsc_queue;

/** cap_pow2 须为 2 的幂且 ≥ 2 */
NTK_API ntk_mpsc_queue *ntk_mpsc_create(size_t cap_pow2);
NTK_API void ntk_mpsc_destroy(ntk_mpsc_queue *q);

/** 多生产者 push；队列满返回 NTK_ERR_AGAIN */
NTK_API ntk_err_t ntk_mpsc_push(ntk_mpsc_queue *q, void *item);
/** 单消费者 pop；空返回 NULL */
NTK_API void *ntk_mpsc_pop(ntk_mpsc_queue *q);

#ifdef __cplusplus
}
#endif

#endif /* NTK_UTIL_MPSC_H */
