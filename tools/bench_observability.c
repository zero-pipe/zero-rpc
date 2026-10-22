/*
 * 可观测性热路径基准：对比 metrics/log 开关前后的单次开销。
 *
 * 关闭 metrics 时 NB_METRIC_* 只剩一次全局标志判断；关闭 log 时
 * nb_log_* 宏短路、不做任何格式化。用于量化“关掉采集后热路径零成本”。
 *
 * 用法：bench_observability [iterations]
 */
#include "atomic.h" /* 内部头：NB_METRIC_* 与运行期开关 */

#include <config.h>
#include <log.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_ns(void)
{
    return (double)clock() * (1e9 / (double)CLOCKS_PER_SEC);
}

static double bench_metric_inc(int enabled, unsigned iters, uint64_t *sink)
{
    uint64_t v = 0;
    unsigned i;
    double t0;
    double t1;

    nb_metrics_enabled = enabled;
    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        NB_METRIC_INC(v);
    }
    t1 = now_ns();
    *sink = v;
    return (t1 - t0) / (double)iters;
}

static double bench_metric_add(int enabled, unsigned iters, uint64_t *sink)
{
    uint64_t v = 0;
    unsigned i;
    double t0;
    double t1;

    nb_metrics_enabled = enabled;
    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        NB_METRIC_ADD(v, 7u);
    }
    t1 = now_ns();
    *sink = v;
    return (t1 - t0) / (double)iters;
}

static double bench_log_guard(int enabled, unsigned iters, uint64_t *sink)
{
    unsigned i;
    uint64_t acc = 0;
    double t0;
    double t1;

    nb_log_configure(enabled, NB_NODE_LOG_INFO);
    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        acc += (uint64_t)nb_log_enabled();
    }
    t1 = now_ns();
    *sink = acc;
    return (t1 - t0) / (double)iters;
}

static double bench_log_disabled_macro(unsigned iters)
{
    unsigned i;
    double t0;
    double t1;

    nb_log_configure(0, NB_NODE_LOG_INFO);
    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        nb_log_debug("bench"); /* 关闭时短路，不格式化、不输出 */
    }
    t1 = now_ns();
    return (t1 - t0) / (double)iters;
}

int main(int argc, char **argv)
{
    unsigned iters = (argc > 1) ? (unsigned)strtoul(argv[1], NULL, 10) : 20000000u;
    uint64_t sink = 0;
    double inc_on;
    double inc_off;
    double add_on;
    double add_off;
    double guard_on;
    double guard_off;
    double log_off;

    if (iters == 0) {
        iters = 1;
    }
    inc_on = bench_metric_inc(1, iters, &sink);
    inc_off = bench_metric_inc(0, iters, &sink);
    add_on = bench_metric_add(1, iters, &sink);
    add_off = bench_metric_add(0, iters, &sink);
    guard_on = bench_log_guard(1, iters, &sink);
    guard_off = bench_log_guard(0, iters, &sink);
    log_off = bench_log_disabled_macro(iters);
    nb_log_configure(1, NB_NODE_LOG_INFO);
    nb_metrics_enabled = 1;

    printf("iterations=%u\n", iters);
    printf("%-16s %12s %12s %8s\n", "path", "on(ns/op)", "off(ns/op)", "on/off");
    printf("%-16s %12.3f %12.3f %8.2f\n",
           "metrics.inc",
           inc_on,
           inc_off,
           inc_off > 0 ? inc_on / inc_off : 0.0);
    printf("%-16s %12.3f %12.3f %8.2f\n",
           "metrics.add",
           add_on,
           add_off,
           add_off > 0 ? add_on / add_off : 0.0);
    printf("%-16s %12.3f %12.3f %8.2f\n",
           "log.guard",
           guard_on,
           guard_off,
           guard_off > 0 ? guard_on / guard_off : 0.0);
    printf("%-16s %12s %12.3f %8s\n", "log.debug(off)", "-", log_off, "-");
    printf("sink=%llu\n", (unsigned long long)sink);
    return 0;
}
