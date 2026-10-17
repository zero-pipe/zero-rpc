#ifndef ZTK_NET_RTP_PORT_H
#define ZTK_NET_RTP_PORT_H

#include "../ztk_export.h"
#include "../ztk_errno.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ztk_rtp_port_pool ztk_rtp_port_pool;

typedef struct ztk_rtp_port_pool_opts {
    /** 偶数 RTP 端口下界（含） */
    uint16_t range_start;
    /** 偶数 RTP 端口上界（含），须 >= range_start 且为偶数 */
    uint16_t range_end;
} ztk_rtp_port_pool_opts_t;

/** 默认范围 30000–39998（步长 2，约 5000 对） */
ZTK_API ztk_rtp_port_pool *ztk_rtp_port_pool_create(const ztk_rtp_port_pool_opts_t *opts);
ZTK_API void ztk_rtp_port_pool_destroy(ztk_rtp_port_pool *pool);

/** 分配一对端口：rtp 为偶数，*rtcp_port = *rtp_port + 1 */
ZTK_API ztk_err_t ztk_rtp_port_pool_acquire(ztk_rtp_port_pool *pool, uint16_t *rtp_port, uint16_t *rtcp_port);

/** 按 RTP 偶数端口释放（须为本池 acquire 所得） */
ZTK_API void ztk_rtp_port_pool_release(ztk_rtp_port_pool *pool, uint16_t rtp_port);

ZTK_API unsigned ztk_rtp_port_pool_free_count(const ztk_rtp_port_pool *pool);

#ifdef __cplusplus
}
#endif

#endif /* ZTK_NET_RTP_PORT_H */
