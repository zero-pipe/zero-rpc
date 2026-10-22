#include "atomic.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            g_fail = 1;                                                      \
        }                                                                    \
    } while (0)

/* metrics 关闭时计数宏必须完全短路；开启后恢复累加。 */
static void test_metrics_switch(void)
{
    uint64_t value = 0;

    g_zri_metrics_enabled = 0;
    ZRI_METRIC_INC(value);
    ZRI_METRIC_ADD(value, 10u);
    ZRI_METRIC_SUB(value, 1u);
    CHECK(value == 0);

    g_zri_metrics_enabled = 1;
    ZRI_METRIC_INC(value);
    ZRI_METRIC_ADD(value, 10u);
    ZRI_METRIC_SUB(value, 1u);
    CHECK(value == 10);
}

/* 延迟开关/采样参数是独立全局，供 dispatch 的直方图路径判断。 */
static void test_latency_switch(void)
{
    g_zri_latency_enabled = 0;
    CHECK(g_zri_latency_enabled == 0);
    g_zri_latency_enabled = 1;
    CHECK(g_zri_latency_enabled == 1);

    g_zri_metrics_sample_every = 5;
    CHECK(g_zri_metrics_sample_every == 5);
    g_zri_metrics_sample_every = 1;
}

int main(void)
{
    test_metrics_switch();
    test_latency_switch();
    if (g_fail) {
        fprintf(stderr, "metrics: FAIL\n");
        return 1;
    }
    printf("metrics: OK\n");
    return 0;
}
