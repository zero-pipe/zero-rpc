#ifndef NTK_NET_NET_LIMITS_H
#define NTK_NET_NET_LIMITS_H

/**
 * ntk 网络/监听策略常量（非协议魔数）
 */
#include "../ntk_export.h"

#define NTK_NET_ADDR_STR_LEN           64
#define NTK_NET_PORT_STR_LEN           16

#define NTK_TCP_DEFAULT_BACKLOG        128
/** TCP 会话发送缓冲初始容量（与 UDP 收包缓冲对齐） */
#define NTK_TCP_SESSION_OUT_INIT_CAP   65536

#define NTK_MS_PER_SEC                 1000

#endif /* NTK_NET_NET_LIMITS_H */
