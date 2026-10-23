/*
 * This file is part of the EasyLogger Library.
 *
 * Copyright (c) 2015, Armink, <armink.ztl@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * 'Software'), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Function: Portable interface for each platform.
 * Created on: 2015-04-28
 */
 
#include <elog.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static CRITICAL_SECTION s_lock;
static int s_lock_ready;
#else
#include <pthread.h>
#include <sys/time.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static char s_time[32];
static char s_thread[32];
static FILE *s_file;
static int s_stderr_enabled = 1;
static int s_flush_enabled = 1;

static int env_flag(const char *name, int default_value) {
    const char *value = getenv(name);
    if (!value || !value[0]) {
        return default_value;
    }
    return strcmp(value, "0") != 0 && strcmp(value, "false") != 0;
}

/**
 * EasyLogger port initialize
 *
 * @return result
 */
ElogErrCode elog_port_init(void) {
#if defined(_WIN32)
    if (!s_lock_ready) {
        InitializeCriticalSection(&s_lock);
        s_lock_ready = 1;
    }
#endif
    s_stderr_enabled = env_flag("RPC_LOG_STDERR", 1);
    s_flush_enabled = env_flag("RPC_LOG_FLUSH", 1);
    {
        const char *path = getenv("RPC_LOG_FILE");
        if (path && path[0]) {
            s_file = fopen(path, "a");
        }
    }
    return ELOG_NO_ERR;
}

/**
 * EasyLogger port deinitialize
 *
 */
void elog_port_deinit(void) {
    if (s_file) {
        (void)fclose(s_file);
        s_file = NULL;
    }
#if defined(_WIN32)
    if (s_lock_ready) {
        DeleteCriticalSection(&s_lock);
        s_lock_ready = 0;
    }
#endif
}

/**
 * output log port interface
 *
 * @param log output of log
 * @param size log size
 */
void elog_port_output(const char *log, size_t size) {
    if (!log || size == 0) {
        return;
    }
    if (s_stderr_enabled) {
        (void)fwrite(log, 1, size, stderr);
        (void)fflush(stderr);
    }
    if (s_file) {
        (void)fwrite(log, 1, size, s_file);
        if (s_flush_enabled) {
            (void)fflush(s_file);
        }
    }
}

/**
 * output lock
 */
void elog_port_output_lock(void) {
#if defined(_WIN32)
    if (s_lock_ready) {
        EnterCriticalSection(&s_lock);
    }
#else
    pthread_mutex_lock(&s_lock);
#endif
}

/**
 * output unlock
 */
void elog_port_output_unlock(void) {
#if defined(_WIN32)
    if (s_lock_ready) {
        LeaveCriticalSection(&s_lock);
    }
#else
    pthread_mutex_unlock(&s_lock);
#endif
}

/**
 * get current time interface
 *
 * @return current time
 */
const char *elog_port_get_time(void) {
#if defined(_WIN32)
    SYSTEMTIME st;
    GetLocalTime(&st);
    (void)snprintf(s_time, sizeof(s_time), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                   (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
                   (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
                   (unsigned)st.wMilliseconds);
#else
    struct timeval tv;
    struct tm tm;
    if (gettimeofday(&tv, NULL) == 0 && localtime_r(&tv.tv_sec, &tm)) {
        (void)snprintf(s_time, sizeof(s_time), "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                       tm.tm_hour, tm.tm_min, tm.tm_sec, (long)(tv.tv_usec / 1000));
    } else {
        s_time[0] = '\0';
    }
#endif
    return s_time;
}

/**
 * get current process name interface
 *
 * @return current process name
 */
const char *elog_port_get_p_info(void) {
    return "rpc";
}

/**
 * get current thread name interface
 *
 * @return current thread name
 */
const char *elog_port_get_t_info(void) {
#if defined(_WIN32)
    (void)snprintf(s_thread, sizeof(s_thread), "%lu", (unsigned long)GetCurrentThreadId());
#else
    (void)snprintf(s_thread, sizeof(s_thread), "%lu",
                   (unsigned long)(uintptr_t)pthread_self());
#endif
    return s_thread;
}
