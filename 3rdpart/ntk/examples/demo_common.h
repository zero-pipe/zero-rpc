#ifndef ZTK_DEMO_COMMON_H
#define ZTK_DEMO_COMMON_H

#include "ztk/platform.h"
#include <stdio.h>

/** 本地无 root 时可编译：-DDEMO_RTSP_PORT=8554 -DDEMO_HTTP_PORT=8080 -DDEMO_RTMP_PORT=1935 */
#ifndef DEMO_RTSP_PORT
#define DEMO_RTSP_PORT 554
#endif
#ifndef DEMO_HTTP_PORT
#define DEMO_HTTP_PORT 80
#endif
#ifndef DEMO_RTMP_PORT
#define DEMO_RTMP_PORT 1935
#endif

static inline void demo_print_banner(const char *demo_id, const char *title)
{
    printf("\n========== [%s] %s ==========\n", demo_id, title);
}

static inline void demo_print_listen(const char *service, uint16_t port)
{
    printf("[%s] listening on 0.0.0.0:%u (recv/disconnect logged below)\n", service, (unsigned)port);
    printf("[%s] test: echo test | nc 127.0.0.1 %u\n", service, (unsigned)port);
}

static inline void demo_on_recv_print(const char *service, const void *data, size_t len)
{
    printf("[%s] received message (%zu bytes): ", service, len);
    size_t show = len < 64 ? len : 64;
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < show; ++i)
        putchar((p[i] >= 32 && p[i] < 127) ? (char)p[i] : '.');
    if (len > show)
        printf("...");
    putchar('\n');
    fflush(stdout);
}

/** TcpServer 在连接关闭/出错时调 on_error（recv 返回 0 或对端 RST 等） */
static inline void demo_on_disconnect_print(const char *service)
{
    printf("[%s] client disconnected\n", service);
    fflush(stdout);
}

static inline void demo_sleep_until_stop(volatile int *stop)
{
    while (!*stop)
        ztk_sleep_ms(200);
}

#endif /* ZTK_DEMO_COMMON_H */
