#include <log.h>

#include <stdlib.h>
#include <string.h>

static uint8_t log_level_from_env(void)
{
    const char *value = getenv("RPC_LOG_LEVEL");
    if (!value || !value[0]) {
        return ELOG_LVL_INFO;
    }
    if (strcmp(value, "assert") == 0) {
        return ELOG_LVL_ASSERT;
    }
    if (strcmp(value, "error") == 0) {
        return ELOG_LVL_ERROR;
    }
    if (strcmp(value, "warn") == 0) {
        return ELOG_LVL_WARN;
    }
    if (strcmp(value, "debug") == 0) {
        return ELOG_LVL_DEBUG;
    }
    if (strcmp(value, "verbose") == 0) {
        return ELOG_LVL_VERBOSE;
    }
    return ELOG_LVL_INFO;
}

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 /* InitOnceExecuteOnce 需要 Vista+ */
#endif
#include <windows.h>

static INIT_ONCE s_log_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK log_init_once(PINIT_ONCE once, PVOID param, PVOID *context)
{
    uint8_t level;
    (void)once;
    (void)param;
    (void)context;

    if (elog_init() != ELOG_NO_ERR) {
        return TRUE;
    }
    for (level = ELOG_LVL_ASSERT; level <= ELOG_LVL_VERBOSE; level++) {
        elog_set_fmt(level, ELOG_FMT_LVL | ELOG_FMT_TAG | ELOG_FMT_TIME);
    }
    elog_set_filter_lvl(log_level_from_env());
    elog_start();
    return TRUE;
}

void rpc_log_init(void)
{
    (void)InitOnceExecuteOnce(&s_log_once, log_init_once, NULL, NULL);
}
#else
#include <pthread.h>

static pthread_once_t s_log_once = PTHREAD_ONCE_INIT;

static void log_init_once(void)
{
    uint8_t level;

    if (elog_init() != ELOG_NO_ERR) {
        return;
    }
    for (level = ELOG_LVL_ASSERT; level <= ELOG_LVL_VERBOSE; level++) {
        elog_set_fmt(level, ELOG_FMT_LVL | ELOG_FMT_TAG | ELOG_FMT_TIME);
    }
    elog_set_filter_lvl(log_level_from_env());
    elog_start();
}

void rpc_log_init(void)
{
    (void)pthread_once(&s_log_once, log_init_once);
}
#endif
