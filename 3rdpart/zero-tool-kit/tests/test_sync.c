#include "ztk/thread/sync.h"
#include "ztk/thread/sem.h"
#include "ztk/thread/thread.h"
#include <stdio.h>

static volatile int g_done;

static void worker(void *user)
{
    ztk_sem *sem = (ztk_sem *)user;
    ztk_sem_post(sem, 1);
}

int main(void)
{
    ztk_mutex *m = ztk_mutex_create(ZTK_MUTEX_RECURSIVE);
    ztk_sem *sem = ztk_sem_create(0);
    if (!m || !sem)
        return 1;

    if (ztk_mutex_lock(m) != ZTK_OK)
        return 1;
    if (ztk_mutex_lock(m) != ZTK_OK) /* recursive */
        return 1;
    if (ztk_mutex_unlock(m) != ZTK_OK || ztk_mutex_unlock(m) != ZTK_OK)
        return 1;

    ztk_thread *t = ztk_thread_create(worker, sem);
    if (!t)
        return 1;

    if (ztk_sem_timedwait(sem, 3000) != ZTK_OK) {
        fprintf(stderr, "sem wait failed\n");
        return 1;
    }

    ztk_thread_join(t);
    ztk_thread_destroy(t);
    ztk_sem_destroy(sem);
    ztk_mutex_destroy(m);
    printf("test_sync ok\n");
    return 0;
}
