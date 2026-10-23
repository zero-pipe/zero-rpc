#include "net/rtp_port.h"
#include <stdlib.h>
#include <string.h>

#define NTK_RTP_PORT_DEFAULT_START 30000u
#define NTK_RTP_PORT_DEFAULT_END   39998u

struct ntk_rtp_port_pool {
    uint16_t range_start;
    uint16_t range_end;
    uint16_t *free_ports;
    unsigned free_cap;
    unsigned free_count;
};

static int port_valid(uint16_t p, uint16_t start, uint16_t end)
{
    return (p >= start && p <= end && (p & 1u) == 0);
}

static int pool_push(ntk_rtp_port_pool *pool, uint16_t port)
{
    if (pool->free_count >= pool->free_cap) {
        unsigned ncap = pool->free_cap ? pool->free_cap * 2u : 64u;
        uint16_t *nbuf = (uint16_t *)realloc(pool->free_ports, ncap * sizeof(uint16_t));
        if (!nbuf)
            return 0;
        pool->free_ports = nbuf;
        pool->free_cap = ncap;
    }
    pool->free_ports[pool->free_count++] = port;
    return 1;
}

static int pool_pop(ntk_rtp_port_pool *pool, uint16_t *port)
{
    if (pool->free_count == 0)
        return 0;
    *port = pool->free_ports[--pool->free_count];
    return 1;
}

ntk_rtp_port_pool *ntk_rtp_port_pool_create(const ntk_rtp_port_pool_opts_t *opts)
{
    uint16_t start = NTK_RTP_PORT_DEFAULT_START;
    uint16_t end = NTK_RTP_PORT_DEFAULT_END;

    if (opts) {
        if (opts->range_start)
            start = opts->range_start;
        if (opts->range_end)
            end = opts->range_end;
    }
    if ((start & 1u) || (end & 1u) || start > end)
        return NULL;

    ntk_rtp_port_pool *pool = (ntk_rtp_port_pool *)calloc(1, sizeof(*pool));
    if (!pool)
        return NULL;

    pool->range_start = start;
    pool->range_end = end;

    for (uint32_t p = start; p <= end; p += 2u) {
        if (!pool_push(pool, (uint16_t)p)) {
            ntk_rtp_port_pool_destroy(pool);
            return NULL;
        }
    }
    return pool;
}

void ntk_rtp_port_pool_destroy(ntk_rtp_port_pool *pool)
{
    if (!pool)
        return;
    free(pool->free_ports);
    free(pool);
}

ntk_err_t ntk_rtp_port_pool_acquire(ntk_rtp_port_pool *pool, uint16_t *rtp_port, uint16_t *rtcp_port)
{
    if (!pool || !rtp_port || !rtcp_port)
        return NTK_ERR_INVALID;

    uint16_t rtp;
    if (!pool_pop(pool, &rtp))
        return NTK_ERR_NOMEM;

    *rtp_port = rtp;
    *rtcp_port = (uint16_t)(rtp + 1u);
    return NTK_OK;
}

void ntk_rtp_port_pool_release(ntk_rtp_port_pool *pool, uint16_t rtp_port)
{
    if (!pool || !port_valid(rtp_port, pool->range_start, pool->range_end))
        return;
    (void)pool_push(pool, rtp_port);
}

unsigned ntk_rtp_port_pool_free_count(const ntk_rtp_port_pool *pool)
{
    return pool ? pool->free_count : 0;
}
