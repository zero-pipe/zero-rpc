#ifndef ZTK_DEMO_UDP_COMMON_H
#define ZTK_DEMO_UDP_COMMON_H

#include "ztk/platform.h"
#include "ztk/poller/poller.h"
#include "ztk/thread/thread.h"
#include <stdio.h>
#include <string.h>

/** 单播本地测试端口 */
#ifndef DEMO_UDP_UNICAST_PORT
#define DEMO_UDP_UNICAST_PORT 9901
#endif

/** 组播（IP multicast） */
#ifndef DEMO_UDP_MCAST_GROUP
#define DEMO_UDP_MCAST_GROUP "239.255.0.1"
#endif
#ifndef DEMO_UDP_MCAST_PORT
#define DEMO_UDP_MCAST_PORT 37020
#endif

/** 广播（SO_BROADCAST）监听端口 */
#ifndef DEMO_UDP_BCAST_PORT
#define DEMO_UDP_BCAST_PORT 9902
#endif

static inline void demo_udp_banner(const char *id, const char *title)
{
    printf("\n========== [UDP-%s] %s ==========\n", id, title);
}

static inline void demo_udp_print_packet(const char *kind, const char *from_ip, uint16_t from_port,
                                         const void *data, size_t len)
{
    printf("[%s] received from %s:%u (%zu bytes): ", kind, from_ip ? from_ip : "?", (unsigned)from_port, len);
    size_t show = len < 48 ? len : 48;
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < show; ++i)
        putchar((p[i] >= 32 && p[i] < 127) ? (char)p[i] : '.');
    if (len > show)
        printf("...");
    putchar('\n');
    fflush(stdout);
}

static inline void demo_udp_print_send(const char *kind, const char *dest, uint16_t port, const char *msg)
{
    printf("[%s] send -> %s:%u  payload: %s\n", kind, dest, (unsigned)port, msg);
    fflush(stdout);
}

typedef struct demo_poller_run {
    ztk_poller *poller;
    volatile int *stop;
} demo_poller_run;

static void demo_poller_thread_entry(void *arg)
{
    demo_poller_run *ctx = (demo_poller_run *)arg;
    ztk_poller_run(ctx->poller, ctx->stop);
}

static inline void demo_run_seconds(volatile int *stop, int seconds)
{
    for (int i = 0; i < seconds * 5 && !*stop; ++i)
        ztk_sleep_ms(200);
}

#endif /* ZTK_DEMO_UDP_COMMON_H */
