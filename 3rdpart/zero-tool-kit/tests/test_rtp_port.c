#include "ztk/net/rtp_port.h"
#include <stdio.h>

int main(void)
{
    ztk_rtp_port_pool_opts_t opts = { 30100, 30120 };
    ztk_rtp_port_pool *pool = ztk_rtp_port_pool_create(&opts);
    if (!pool)
        return 1;

    unsigned initial = ztk_rtp_port_pool_free_count(pool);
    if (initial != 11)
        return 2;

    uint16_t rtp1, rtcp1, rtp2, rtcp2;
    if (ztk_rtp_port_pool_acquire(pool, &rtp1, &rtcp1) != ZTK_OK)
        return 3;
    if (ztk_rtp_port_pool_acquire(pool, &rtp2, &rtcp2) != ZTK_OK)
        return 4;
    if (rtcp1 != (uint16_t)(rtp1 + 1) || rtcp2 != (uint16_t)(rtp2 + 1))
        return 5;
    if (rtp1 == rtp2)
        return 6;

    ztk_rtp_port_pool_release(pool, rtp1);
    ztk_rtp_port_pool_release(pool, rtp2);

    if (ztk_rtp_port_pool_free_count(pool) != initial)
        return 7;

    ztk_rtp_port_pool_destroy(pool);
    printf("test_rtp_port ok\n");
    return 0;
}
