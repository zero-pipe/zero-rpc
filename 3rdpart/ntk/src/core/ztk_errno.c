#include "ztk/ztk_errno.h"
#include <string.h>

static const char *const k_msgs[] = {
    "ok",
    "out of memory",
    "invalid argument",
    "I/O error",
    "timeout",
    "try again",
    "not implemented",
    "platform error",
    "invalid state",
    "buffer too small"
};

#if defined(_MSC_VER)
static __declspec(thread) char s_last[256];
#else
static __thread char s_last[256];
#endif

const char *ztk_strerror(ztk_err_t err)
{
    int idx = -err;
    if (idx >= 0 && idx < (int)(sizeof(k_msgs) / sizeof(k_msgs[0])))
        return k_msgs[idx];
    return "unknown error";
}

void ztk_set_last_error(const char *msg)
{
    if (!msg) {
        s_last[0] = '\0';
        return;
    }
    strncpy(s_last, msg, sizeof(s_last) - 1);
    s_last[sizeof(s_last) - 1] = '\0';
}

const char *ztk_last_error(void)
{
    return s_last;
}
