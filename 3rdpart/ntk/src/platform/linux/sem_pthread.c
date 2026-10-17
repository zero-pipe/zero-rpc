#include "ztk/thread/sem.h"
#include "ztk/ztk_errno.h"
#include "ztk_config.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#if defined(ZTK_HAVE_POSIX_SEM)
#  include <semaphore.h>
#endif

struct ztk_sem {
#if defined(ZTK_HAVE_POSIX_SEM)
    sem_t native;
#else
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    unsigned int count;
#endif
};

ztk_sem *ztk_sem_create(unsigned int initial)
{
    ztk_sem *s = (ztk_sem *)calloc(1, sizeof(*s));
    if (!s)
        return NULL;

#if defined(ZTK_HAVE_POSIX_SEM)
    if (sem_init(&s->native, 0, initial) != 0) {
        free(s);
        return NULL;
    }
#else
    pthread_mutex_init(&s->mtx, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->count = initial;
#endif
    return s;
}

void ztk_sem_destroy(ztk_sem *s)
{
    if (!s)
        return;
#if defined(ZTK_HAVE_POSIX_SEM)
    sem_destroy(&s->native);
#else
    pthread_mutex_destroy(&s->mtx);
    pthread_cond_destroy(&s->cond);
#endif
    free(s);
}

static ztk_err_t sem_wait_impl(ztk_sem *s, int timeout_ms)
{
#if defined(ZTK_HAVE_POSIX_SEM)
    if (timeout_ms < 0) {
        while (sem_wait(&s->native) != 0) {
            if (errno != EINTR)
                return ZTK_ERR_PLATFORM;
        }
        return ZTK_OK;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    int r;
    do {
        r = sem_timedwait(&s->native, &ts);
    } while (r != 0 && errno == EINTR);
    if (r == 0)
        return ZTK_OK;
    if (errno == ETIMEDOUT)
        return ZTK_ERR_TIMEOUT;
    return ZTK_ERR_PLATFORM;
#else
    pthread_mutex_lock(&s->mtx);
    if (timeout_ms < 0) {
        while (s->count == 0)
            pthread_cond_wait(&s->cond, &s->mtx);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        while (s->count == 0) {
            int r = pthread_cond_timedwait(&s->cond, &s->mtx, &ts);
            if (r == ETIMEDOUT) {
                pthread_mutex_unlock(&s->mtx);
                return ZTK_ERR_TIMEOUT;
            }
            if (r != 0 && r != EINTR) {
                pthread_mutex_unlock(&s->mtx);
                return ZTK_ERR_PLATFORM;
            }
        }
    }
    s->count--;
    pthread_mutex_unlock(&s->mtx);
    return ZTK_OK;
#endif
}

ztk_err_t ztk_sem_wait(ztk_sem *s)
{
    if (!s)
        return ZTK_ERR_INVALID;
    return sem_wait_impl(s, -1);
}

ztk_err_t ztk_sem_timedwait(ztk_sem *s, unsigned int timeout_ms)
{
    if (!s)
        return ZTK_ERR_INVALID;
    return sem_wait_impl(s, (int)timeout_ms);
}

ztk_err_t ztk_sem_post(ztk_sem *s, unsigned int count)
{
    if (!s || count == 0)
        return ZTK_ERR_INVALID;

#if defined(ZTK_HAVE_POSIX_SEM)
    for (unsigned int i = 0; i < count; ++i) {
        if (sem_post(&s->native) != 0)
            return ZTK_ERR_PLATFORM;
    }
    return ZTK_OK;
#else
    pthread_mutex_lock(&s->mtx);
    s->count += count;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->mtx);
    return ZTK_OK;
#endif
}
