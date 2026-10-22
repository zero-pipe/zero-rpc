#include <log.h>

#include <stdlib.h>
#include <string.h>

static uint8_t nb_log_level_from_env(void)
{
    const char *value = getenv("NB_LOG_LEVEL");
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

/* 运行期开关；默认开启，nb_node_create 按配置覆盖。 */
static volatile int nb_log_gate = 1;
/* 初始化只做一次；之后 nb_log_enabled 只读该标志，避免每次日志都进 once。 */
static volatile int nb_s_log_inited = 0;

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 /* InitOnceExecuteOnce 需要 Vista+ */
#endif
#include <windows.h>

static INIT_ONCE nb_s_log_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK nb_log_init_once(PINIT_ONCE once, PVOID param, PVOID *context)
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
    elog_set_filter_lvl(nb_log_level_from_env());
    elog_start();
    return TRUE;
}

void nb_log_init(void)
{
    (void)InitOnceExecuteOnce(&nb_s_log_once, nb_log_init_once, NULL, NULL);
}

void nb_log_configure(int enabled, uint8_t level)
{
    const char *env;

    nb_log_init();
    nb_s_log_inited = 1;
    nb_log_gate = enabled ? 1 : 0;
    /* 保留 NB_LOG_LEVEL 的调试优先级，未设置时使用配置级别。 */
    env = getenv("NB_LOG_LEVEL");
    elog_set_filter_lvl(env && env[0] ? nb_log_level_from_env() : level);
}

int nb_log_enabled(void)
{
    if (!nb_s_log_inited) {
        nb_log_init();
        nb_s_log_inited = 1;
    }
    return nb_log_gate;
}
#else
#include <pthread.h>

static pthread_once_t nb_s_log_once = PTHREAD_ONCE_INIT;

static void nb_log_init_once(void)
{
    uint8_t level;

    if (elog_init() != ELOG_NO_ERR) {
        return;
    }
    for (level = ELOG_LVL_ASSERT; level <= ELOG_LVL_VERBOSE; level++) {
        elog_set_fmt(level, ELOG_FMT_LVL | ELOG_FMT_TAG | ELOG_FMT_TIME);
    }
    elog_set_filter_lvl(nb_log_level_from_env());
    elog_start();
}

void nb_log_init(void)
{
    (void)pthread_once(&nb_s_log_once, nb_log_init_once);
}

void nb_log_configure(int enabled, uint8_t level)
{
    const char *env;

    nb_log_init();
    nb_s_log_inited = 1;
    nb_log_gate = enabled ? 1 : 0;
    env = getenv("NB_LOG_LEVEL");
    elog_set_filter_lvl(env && env[0] ? nb_log_level_from_env() : level);
}

int nb_log_enabled(void)
{
    if (!nb_s_log_inited) {
        nb_log_init();
        nb_s_log_inited = 1;
    }
    return nb_log_gate;
}
#endif
