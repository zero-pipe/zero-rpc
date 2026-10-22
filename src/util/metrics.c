/* metrics 运行期开关的全局状态（见 atomic.h / config.h）。 */

#include "atomic.h"

#include <metrics.h>

volatile int g_zri_metrics_enabled = 1;
volatile int g_zri_latency_enabled = 1;
volatile uint32_t g_zri_metrics_sample_every = 1;

/* 延迟直方图分桶单一真源，node.c / dispatch.c 共用。 */
const uint32_t zri_latency_bounds[LATENCY_HIST_BUCKETS - 1] = {
    1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000
};
