#include "ztk/ztk.h"
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    ztk_platform_init();
    ztk_platform_info_t info;
    ztk_platform_get_info(&info);

    if (!info.name || info.name[0] == '\0') {
        fprintf(stderr, "platform name empty\n");
        return 1;
    }

    uint64_t t0 = ztk_monotonic_ms();
    ztk_sleep_ms(10);
    uint64_t t1 = ztk_monotonic_ms();
    if (t1 < t0 + 5) {
        fprintf(stderr, "monotonic/sleep unexpected: %llu %llu\n",
                (unsigned long long)t0, (unsigned long long)t1);
        return 1;
    }

    printf("platform=%s version=%s cores=%u ok\n",
           info.name,
           ztk_version_string(),
           ztk_thread_hardware_concurrency());
    return 0;
}
