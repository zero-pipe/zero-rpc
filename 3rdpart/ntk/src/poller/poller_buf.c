#include "poller_internal.h"
#include "util/buf.h"

/* defined in util/buf.c — bind lockless pool owner for cross-thread release */
void ntk_buf_pool_set_owner_poller(ntk_buf_pool *pool, ntk_poller *owner);

void ntk_poller_attach_buf_pool(ntk_poller *poller, ntk_buf_pool *pool)
{
    if (!poller) {
        return;
    }
    if (poller->buf_pool && poller->buf_pool != pool) {
        ntk_buf_pool_set_owner_poller(poller->buf_pool, NULL);
    }
    poller->buf_pool = pool;
    if (pool) {
        ntk_buf_pool_set_owner_poller(pool, poller);
    }
}

ntk_buf_pool *ntk_poller_buf_pool(ntk_poller *poller)
{
    return poller ? poller->buf_pool : NULL;
}
