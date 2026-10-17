#ifndef ZTK_TEST_PORTABLE_H
#define ZTK_TEST_PORTABLE_H

#include "ztk/platform.h"

/** 测试用短睡眠（毫秒），跨 Linux / Win32 */
static inline void ztk_test_sleep_ms(unsigned ms)
{
    ztk_sleep_ms(ms);
}

#endif /* ZTK_TEST_PORTABLE_H */
