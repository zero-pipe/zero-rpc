#ifndef ZTK_NET_NET_LIMITS_H
#define ZTK_NET_NET_LIMITS_H

/**
 * ztk 网络/监听策略常量（非协议魔数）
 */
#include "../ztk_export.h"

#define ZTK_NET_ADDR_STR_LEN           64
#define ZTK_NET_PORT_STR_LEN           16

#define ZTK_TCP_DEFAULT_BACKLOG        128
/** TCP 会话发送缓冲初始容量（与 UDP 收包缓冲对齐） */
#define ZTK_TCP_SESSION_OUT_INIT_CAP   65536

#define ZTK_MS_PER_SEC                 1000

#endif /* ZTK_NET_NET_LIMITS_H */
