#include "atomic.h"

#include <metrics.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

/* 指标登记表：名称/类型/单位可查，延迟分桶有唯一来源。 */
static void test_metric_registry(void)
{
    const nb_metric_desc_t *desc;
    const uint32_t *bounds = NULL;
    size_t count;

    CHECK(nb_metric_table_count > 0);
    desc = nb_metric_find("requests_sent_total");
    CHECK(desc != NULL);
    CHECK(desc->type == NB_METRIC_COUNTER);
    CHECK(strcmp(desc->unit, "requests") == 0);
    CHECK(desc->offset == offsetof(metrics_t, requests_sent));

    desc = nb_metric_find("connections");
    CHECK(desc != NULL);
    CHECK(desc->type == NB_METRIC_GAUGE);

    desc = nb_metric_find("latency_ms");
    CHECK(desc != NULL);
    CHECK(desc->type == NB_METRIC_HISTOGRAM);
    CHECK(desc->offset == offsetof(metrics_t, latency_hist));

    CHECK(nb_metric_find("does_not_exist") == NULL);
    count = nb_metric_latency_bounds(&bounds);
    CHECK(count == LATENCY_HIST_BUCKETS - 1);
    CHECK(bounds != NULL);
    CHECK(bounds[0] == 1u);
    CHECK(bounds[count - 1] == 10000u);
}

int main(void)
{
    test_metrics_switch();
    test_latency_switch();
    test_metric_registry();
    if (g_fail) {
        fprintf(stderr, "metrics: FAIL\n");
        return 1;
    }
    printf("metrics: OK\n");
    return 0;
}
