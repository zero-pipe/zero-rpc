#include "ztk/poller/poller.h"
#include "ztk/thread/thread.h"
#include <stdio.h>

static volatile int g_wake_count;

static void on_wake(int fd, unsigned events, void *user)
{
    (void)fd;
    (void)events;
    (void)user;
    g_wake_count++;
}

static void wake_thread(void *user)
{
    ztk_poller *p = (ztk_poller *)user;
    ztk_poller_wake(p);
}

int main(void)
{
    ztk_poller *p = ztk_poller_create();
    if (!p) {
        fprintf(stderr, "poller_create failed\n");
        return 1;
    }

    int wake_r = ztk_poller_wake_read_fd(p);
    if (ztk_poller_add(p, wake_r, ZTK_POLL_IN, on_wake, NULL) != ZTK_OK) {
        fprintf(stderr, "poller_add wake failed\n");
        ztk_poller_destroy(p);
        return 1;
    }

    ztk_thread *t = ztk_thread_create(wake_thread, p);
    if (!t) {
        ztk_poller_destroy(p);
        return 1;
    }

    for (int i = 0; i < 50 && g_wake_count == 0; ++i) {
        int n = ztk_poller_poll(p, 100);
        if (n < 0) {
            fprintf(stderr, "poll failed\n");
            ztk_poller_destroy(p);
            return 1;
        }
    }

    ztk_thread_join(t);
    ztk_thread_destroy(t);
    ztk_poller_destroy(p);

    if (g_wake_count < 1) {
        fprintf(stderr, "no wake received\n");
        return 1;
    }
    printf("test_poller ok\n");
    return 0;
}
