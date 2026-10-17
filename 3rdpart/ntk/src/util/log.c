#include "ztk/util/log.h"
#include "ztk/platform.h"
#include <stdio.h>
#include <string.h>

/* ── 状态 ── */
static ztk_log_level_t s_level      = ZTK_LOG_INFO;
static FILE           *s_log_file   = NULL;

/* rotate 配置（0 = 不轮转） */
static size_t s_rotate_max_bytes  = 0;
static int    s_rotate_keep_count = 3;

/* 当前文件已写字节数（近似；每行写入后累加，不做 ftell） */
static size_t s_written_bytes = 0;

/* 打开时记录路径，供 rotate 使用 */
#define ZTK_LOG_PATH_MAX 512
static char s_log_path[ZTK_LOG_PATH_MAX] = {0};

/* ── 内部：执行一次 rotate ── */
static void do_rotate(void)
{
    char old_path[ZTK_LOG_PATH_MAX + 16];
    char new_path[ZTK_LOG_PATH_MAX + 16];
    int  i;

    if (!s_log_path[0])
        return;

    /* 关闭当前文件 */
    if (s_log_file) {
        fclose(s_log_file);
        s_log_file = NULL;
    }

    /* 向后移：.log.(N-1) → .log.N，…，.log.1 → .log.2，.log → .log.1 */
    for (i = s_rotate_keep_count - 1; i >= 1; --i) {
        snprintf(old_path, sizeof(old_path), "%s.%d", s_log_path, i);
        snprintf(new_path, sizeof(new_path), "%s.%d", s_log_path, i + 1);
        rename(old_path, new_path);  /* 忽略不存在的文件错误 */
    }
    /* 当前文件 → .log.1 */
    snprintf(new_path, sizeof(new_path), "%s.1", s_log_path);
    rename(s_log_path, new_path);

    /* 打开新文件 */
    s_log_file     = fopen(s_log_path, "a");
    s_written_bytes = 0;
}

/* ── 公开 API ── */

int ztk_log_open_file(const char *path)
{
    if (!path || !path[0])
        return -1;
    if (s_log_file) {
        fclose(s_log_file);
        s_log_file = NULL;
    }
    strncpy(s_log_path, path, sizeof(s_log_path) - 1);
    s_log_path[sizeof(s_log_path) - 1] = '\0';
    s_log_file      = fopen(path, "a");
    s_written_bytes = 0;
    return s_log_file ? 0 : -1;
}

void ztk_log_close_file(void)
{
    if (s_log_file) {
        fclose(s_log_file);
        s_log_file = NULL;
    }
    s_log_path[0]   = '\0';
    s_written_bytes = 0;
}

void ztk_log_reopen_file(void)
{
    if (!s_log_path[0])
        return;
    if (s_log_file) {
        fclose(s_log_file);
        s_log_file = NULL;
    }
    s_log_file      = fopen(s_log_path, "a");
    s_written_bytes = 0;
}

void ztk_log_set_rotate(size_t max_bytes, int keep_count)
{
    s_rotate_max_bytes  = max_bytes;
    s_rotate_keep_count = keep_count > 0 ? keep_count : 1;
}

static const char *level_name(ztk_log_level_t l)
{
    switch (l) {
    case ZTK_LOG_TRACE: return "T";
    case ZTK_LOG_DEBUG: return "D";
    case ZTK_LOG_INFO:  return "I";
    case ZTK_LOG_WARN:  return "W";
    case ZTK_LOG_ERROR: return "E";
    default:            return "?";
    }
}

void ztk_log_set_level(ztk_log_level_t level)
{
    s_level = level;
}

void ztk_log_writev(ztk_log_level_t level, const char *file, int line, const char *fmt, va_list ap)
{
    if (level < s_level)
        return;

    const char *base = file ? strrchr(file, '/') : NULL;
    if (!base && file)
        base = strrchr(file, '\\');
    if (base)
        base++;
    else
        base = file ? file : "?";

    char prefix[128];
    char msg[4096];
    int  plen;

    plen = snprintf(prefix, sizeof(prefix), "[%s][%llu][%s:%d] ",
        level_name(level),
        (unsigned long long)ztk_monotonic_ms(),
        base,
        line);
    (void)plen;

    va_list ap_copy;
    va_copy(ap_copy, ap);
    vsnprintf(msg, sizeof(msg), fmt, ap_copy);
    va_end(ap_copy);

    fprintf(stderr, "%s%s\n", prefix, msg);
    fflush(stderr);

    if (s_log_file) {
        size_t line_len = strlen(prefix) + strlen(msg) + 1; /* +1 for '\n' */
        fprintf(s_log_file, "%s%s\n", prefix, msg);
        fflush(s_log_file);

        /* 累计写入量；超限则 rotate */
        s_written_bytes += line_len;
        if (s_rotate_max_bytes > 0 && s_written_bytes >= s_rotate_max_bytes)
            do_rotate();
    }
}

void ztk_log_write(ztk_log_level_t level, const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ztk_log_writev(level, file, line, fmt, ap);
    va_end(ap);
}
