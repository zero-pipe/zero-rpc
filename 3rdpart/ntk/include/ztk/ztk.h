#ifndef ZTK_H
#define ZTK_H

/** ZTK 总头文件：包含所有公共模块 */
#include "ztk_export.h"
#include "ztk_errno.h"
#include "platform.h"
#include "thread/sync.h"
#include "thread/sem.h"
#include "thread/thread.h"
#include "util/log.h"
#include "util/byte_buf.h"
#include "util/buf.h"
#include "util/mpsc.h"
#include "util/timer.h"
#include "poller/poller.h"
#include "poller/poller_pool.h"
#include "thread/thread_pool.h"
#include "net/socket.h"
#include "net/tcp_server.h"
#include "net/tcp_client.h"
#include "net/udp_server.h"
#include "net/udp_client.h"

#define ZTK_VERSION_STRING "0.8.0-m8"

#ifdef __cplusplus
extern "C" {
#endif

ZTK_API const char *ztk_version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* ZTK_H */
