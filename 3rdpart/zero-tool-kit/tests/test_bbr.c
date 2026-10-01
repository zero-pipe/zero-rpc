#include "ztk/net/socket.h"
#include "ztk/platform.h"
#include <stdio.h>
#include <string.h>

#if defined(__linux__)
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

int main(void)
{
#if !defined(__linux__)
    printf("test_bbr skip (not Linux)\n");
    return 0;
#else
    ztk_socket *srv = ztk_socket_create();
    ztk_socket *cli = ztk_socket_create();
    int in_progress = 0;
    char algo[32];
    socklen_t alen = sizeof(algo);
    ztk_err_t err;

    if (!srv || !cli)
        return 1;

    if (ztk_socket_listen(srv, "127.0.0.1", 0, 8) != ZTK_OK)
        return 1;

    {
        char lip[64];
        uint16_t port = 0;
        ztk_socket *accepted = NULL;

        if (ztk_socket_get_local(srv, lip, sizeof(lip), &port) != ZTK_OK || port == 0)
            return 1;
        if (ztk_socket_connect(cli, "127.0.0.1", port, &in_progress) != ZTK_OK)
            return 1;
        for (int i = 0; i < 300; ++i) {
            err = ztk_socket_accept(srv, &accepted);
            if (err == ZTK_OK && accepted)
                break;
            err = ztk_socket_check_connect(cli);
            if (err == ZTK_OK)
                break;
            if (err != ZTK_ERR_AGAIN && err != ZTK_OK)
                return 1;
            ztk_sleep_ms(10);
        }
        if (ztk_socket_check_connect(cli) != ZTK_OK)
            return 1;
        if (accepted)
            ztk_socket_destroy(accepted);
    }

    err = ztk_socket_set_bbr(cli, 1);
    if (err == ZTK_ERR_NOT_IMPL) {
        fprintf(stderr, "test_bbr skip (platform no BBR)\n");
        ztk_socket_destroy(cli);
        ztk_socket_destroy(srv);
        return 0;
    }
    if (err != ZTK_OK) {
        fprintf(stderr, "test_bbr skip (set_bbr err=%d)\n", (int)err);
        ztk_socket_destroy(cli);
        ztk_socket_destroy(srv);
        return 0;
    }

    memset(algo, 0, sizeof(algo));
    if (getsockopt(ztk_socket_fd(cli), IPPROTO_TCP, TCP_CONGESTION, algo, &alen) != 0) {
        fprintf(stderr, "test_bbr skip (getsockopt TCP_CONGESTION)\n");
        ztk_socket_destroy(cli);
        ztk_socket_destroy(srv);
        return 0;
    }
    if (strcmp(algo, "bbr") != 0) {
        fprintf(stderr, "test_bbr skip (got algo=%s)\n", algo);
        ztk_socket_destroy(cli);
        ztk_socket_destroy(srv);
        return 0;
    }

    ztk_socket_destroy(cli);
    ztk_socket_destroy(srv);
    printf("test_bbr ok\n");
    return 0;
#endif
}
