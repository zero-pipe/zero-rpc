#include "poller_internal.h"
#include "ztk/util/buf.h"

/* defined in util/buf.c — bind lockless pool owner for cross-thread release */
void ztk_buf_pool_set_owner_poller(ztk_buf_pool *pool, ztk_poller *owner);

void ztk_poller_attach_buf_pool(ztk_poller *poller, ztk_buf_pool *pool)
{
    if (!poller) {
        return;
    }
    if (poller->buf_pool && poller->buf_pool != pool) {
        ztk_buf_pool_set_owner_poller(poller->buf_pool, NULL);
    }
    poller->buf_pool = pool;
    if (pool) {
        ztk_buf_pool_set_owner_poller(pool, poller);
    }
}

ztk_buf_pool *ztk_poller_buf_pool(ztk_poller *poller)
{
    return poller ? poller->buf_pool : NULL;
}
