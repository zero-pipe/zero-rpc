#ifndef NTK_NET_RTP_PORT_H
#define NTK_NET_RTP_PORT_H

#include "../ntk_export.h"
#include "../ntk_errno.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ntk_rtp_port_pool ntk_rtp_port_pool;

typedef struct ntk_rtp_port_pool_opts {
    /** 偶数 RTP 端口下界（含） */
    uint16_t range_start;
    /** 偶数 RTP 端口上界（含），须 >= range_start 且为偶数 */
    uint16_t range_end;
} ntk_rtp_port_pool_opts_t;

/** 默认范围 30000–39998（步长 2，约 5000 对） */
NTK_API ntk_rtp_port_pool *ntk_rtp_port_pool_create(const ntk_rtp_port_pool_opts_t *opts);
NTK_API void ntk_rtp_port_pool_destroy(ntk_rtp_port_pool *pool);

/** 分配一对端口：rtp 为偶数，*rtcp_port = *rtp_port + 1 */
NTK_API ntk_err_t ntk_rtp_port_pool_acquire(ntk_rtp_port_pool *pool, uint16_t *rtp_port, uint16_t *rtcp_port);

/** 按 RTP 偶数端口释放（须为本池 acquire 所得） */
NTK_API void ntk_rtp_port_pool_release(ntk_rtp_port_pool *pool, uint16_t rtp_port);

NTK_API unsigned ntk_rtp_port_pool_free_count(const ntk_rtp_port_pool *pool);

#ifdef __cplusplus
}
#endif

#endif /* NTK_NET_RTP_PORT_H */
