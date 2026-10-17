#include <log.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static INIT_ONCE s_log_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK log_init_once(PINIT_ONCE once, PVOID param, PVOID *context) {
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
    elog_set_filter_lvl(ELOG_LVL_INFO);
    elog_start();
    return TRUE;
}

void rpc_log_init(void) {
    (void)InitOnceExecuteOnce(&s_log_once, log_init_once, NULL, NULL);
}
#else
#include <pthread.h>

static pthread_once_t s_log_once = PTHREAD_ONCE_INIT;

static void log_init_once(void) {
    uint8_t level;

    if (elog_init() != ELOG_NO_ERR) {
        return;
    }
    for (level = ELOG_LVL_ASSERT; level <= ELOG_LVL_VERBOSE; level++) {
        elog_set_fmt(level, ELOG_FMT_LVL | ELOG_FMT_TAG | ELOG_FMT_TIME);
    }
    elog_set_filter_lvl(ELOG_LVL_INFO);
    elog_start();
}

void rpc_log_init(void) {
    (void)pthread_once(&s_log_once, log_init_once);
}
#endif
