/*
 * rpc 压测客户端（对标 fastcall-c tests/perf/framework/rpc_loadgen.cpp 的
 * CLI 与结果 JSON 契约，便于复用 rpc_bench.py 的聚合/报告）。
 *
 * 形状：单进程单"热连接"，同一时刻 1 个 outstanding RPC（对齐 gRPC 单 channel）。
 */
#include <client.h>
#include <platform.h>
#include <thread/sem.h>

#define RPC_LOG_TAG "loadgen"
#include <log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static uint64_t now_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&counter);
    return (uint64_t)((double)counter.QuadPart * 1000000000.0 / (double)freq.QuadPart);
}
#else
#include <time.h>
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

/*
 * 延迟直方图：小延迟用 1µs 线性桶，大延迟用 log2 分桶，避免高延迟全部被
 * 合并进单一哨兵值。数组索引与代表值（µs）的映射见 lat_hist_index/key()。
 */
#define LAT_LINEAR_MAX 65536 /* [0,65535] 为 1µs 线性桶 */
#define LAT_LOG_BUCKETS 24   /* 覆盖约 65ms .. 数小时 */
#define LAT_HIST_SIZE (LAT_LINEAR_MAX + LAT_LOG_BUCKETS)

/* 延迟(µs) -> 直方图桶索引 */
static int lat_hist_index(uint64_t us) {
    int b = 0;
    if (us < LAT_LINEAR_MAX) {
        return (int)us;
    }
    while ((us >> (b + 1)) != 0) {
        b++;
    }
    b -= 16; /* floor(log2(us)) - floor(log2(65536)) */
    if (b >= LAT_LOG_BUCKETS) {
        b = LAT_LOG_BUCKETS - 1;
    }
    return LAT_LINEAR_MAX + b;
}

/* 直方图桶索引 -> 代表值(µs)：线性桶取精确值，对数桶取区间中点 */
static uint64_t lat_hist_key(int idx) {
    int k;
    uint64_t lower;
    uint64_t upper;
    if (idx < LAT_LINEAR_MAX) {
        return (uint64_t)idx;
    }
    k = idx - LAT_LINEAR_MAX;
    lower = (uint64_t)1 << (16 + k);
    upper = (lower << 1) - 1;
    return lower + (upper - lower) / 2;
}

typedef struct options {
    const char *service;
    const char *method;
    const char *worker_id;
    const char *output;
    uint64_t duration_seconds;
    uint64_t max_requests;
    size_t payload_bytes;
    size_t expect_response_bytes;
    uint64_t timeout_ms;
    double rate;
    int verify_response;
    uint32_t connect_timeout_seconds;
    uint16_t discovery_port;
    uint16_t data_port;
    const char *bind;
    const char *peer_ip;
    uint16_t peer_port;
    const char *target_ip;
    uint16_t target_port;
    uint16_t frag_bytes;
    uint32_t max_msg_bytes;
    uint32_t backpressure_bytes;
    transport_kind_t transport;
    uint64_t window; /* 0 = 收一发一；>0 = 滑动窗口在途上限 */
} options_t;

typedef struct callout {
    ntk_sem *sem;
    int status;
    int app_status;
    size_t resp_len;
    int verify_ok;
    uint64_t send_ns;
    uint64_t latency_ns;
} callout_t;

typedef struct error_entry {
    int code;
    uint64_t count;
} error_entry_t;

static int g_status;
static const uint8_t *g_expect_data;
static size_t g_expect_len;

static void on_response(int status, const response_t *resp, void *user) {
    callout_t *co = (callout_t *)user;
    co->status = status;
    co->app_status = resp ? resp->status : status;
    co->resp_len = (status == OK && resp) ? resp->payload.len : 0;
    co->latency_ns = now_ns() - co->send_ns;
    co->verify_ok = 1;
    if (g_expect_data && status == OK && resp) {
        co->verify_ok = (resp->payload.len == g_expect_len &&
                         memcmp(resp->payload.data, g_expect_data, g_expect_len) == 0);
    }
    ntk_sem_post(co->sem, 1);
}

static uint64_t sleep_until_ns(uint64_t deadline) {
    for (;;) {
        uint64_t now = now_ns();
        uint64_t remain;
        if (now >= deadline) {
            return now;
        }
        remain = deadline - now;
        if (remain > 2000000ull) {
            ntk_sleep_ms((unsigned)((remain - 1000000ull) / 1000000ull));
        } else {
            /* 自旋等待亚毫秒精度 */
        }
    }
}

static int parse_u64(const char *s, uint64_t *out) {
    char *end = NULL;
    unsigned long long v;
    if (!s || !s[0]) {
        return 0;
    }
    v = strtoull(s, &end, 10);
    if (!end || *end != '\0') {
        return 0;
    }
    *out = (uint64_t)v;
    return 1;
}

static int parse_size(const char *s, size_t *out) {
    uint64_t v;
    if (!parse_u64(s, &v)) {
        return 0;
    }
    *out = (size_t)v;
    return 1;
}

static void usage(const char *argv0) {
    rpc_log_error(
        "usage: %s [--service s] [--method m] [--duration-seconds n]\n"
        "          [--transport udp|tcp]\n"
        "          [--max-requests n] [--payload-bytes n] [--expect-response-bytes n]\n"
        "          [--timeout-ms n] [--rate qps] [--window n] [--verify-response]\n"
        "          [--connect-timeout-seconds n] [--worker-id id] [--output path|-]\n"
        "          [--mode static|mesh]\n"
        "          [--discovery-port p] [--data-port p] [--max-msg-bytes n] [--bind ip] [--peer host:port]",
        argv0);
}

static void error_add(error_entry_t *errors, int *error_count, int code) {
    int i;
    for (i = 0; i < *error_count; i++) {
        if (errors[i].code == code) {
            errors[i].count++;
            return;
        }
    }
    if (*error_count < 16) {
        errors[*error_count].code = code;
        errors[*error_count].count = 1;
        (*error_count)++;
    }
}

/* ---- 事件驱动（closed-loop）：全程在 poller 线程内联发送，无跨线程/sem 开销 ---- */

static node_t *g_node;
static proxy_t *g_proxy;
static ntk_sem *g_done;
static options_t g_opt;
static const uint8_t *g_payload;
static uint64_t g_start_ns, g_deadline_ns;
static uint64_t g_attempted, g_succeeded, g_failed, g_response_bytes;
static uint64_t g_min_ns, g_max_ns, g_sum_ns, g_lat_count;
static uint64_t g_lat_hist[LAT_HIST_SIZE];
static error_entry_t g_errors[16];
static int g_error_count;
static uint64_t g_window;        /* 0 = 收一发一；>0 = 滑动窗口在途上限 */
static uint64_t g_inflight;      /* 当前在途请求数 */
static uint64_t g_inflight_peak; /* 在途峰值 */
static int g_finished;
static int g_in_issue;
static int g_pause; /* 内联回调要求外层填充循环暂停 */

static void ed_issue(void *user);

static void ed_finish(void) {
    if (g_finished) {
        return;
    }
    g_finished = 1;
    ntk_sem_post(g_done, 1);
}

static void ed_resp(int status, const response_t *resp, void *user) {
    int inline_cb = g_in_issue;
    uint64_t send_ns = (uint64_t)(uintptr_t)user;
    uint64_t lat = now_ns() - send_ns;
    uint64_t us = (lat + 999ull) / 1000ull;
    uint64_t hidx = (uint64_t)lat_hist_index(us);
    int ok;

    if (g_inflight > 0) {
        g_inflight--;
    }

    g_lat_hist[hidx]++;

    ok = (status == OK) && resp && resp->status == OK &&
         (g_opt.expect_response_bytes == 0 ||
          (resp && resp->payload.len == g_opt.expect_response_bytes));
    if (ok && g_opt.verify_response && resp) {
        ok = (resp->payload.len == g_opt.payload_bytes &&
              memcmp(resp->payload.data, g_payload, g_opt.payload_bytes) == 0);
    }
    if (ok) {
        g_succeeded++;
        g_response_bytes += resp ? resp->payload.len : 0;
        if (g_lat_count == 0 || lat < g_min_ns) {
            g_min_ns = lat;
        }
        if (lat > g_max_ns) {
            g_max_ns = lat;
        }
        g_sum_ns += lat;
        g_lat_count++;
    } else {
        g_failed++;
        error_add(g_errors, &g_error_count,
                  status != OK ? status : (resp ? resp->status : ERR_STATE));
    }

    if (now_ns() >= g_deadline_ns ||
        (g_opt.max_requests != 0 && g_attempted >= g_opt.max_requests)) {
        if (g_inflight == 0) {
            ed_finish();
        } else if (inline_cb) {
            g_pause = 1;
        }
        return;
    }
    if (status == ERR_NOTFOUND) {
        /* 尚未发现：稍后重试，避免忙转 */
        if (inline_cb) {
            g_pause = 1;
        }
        (void)node_post_delay(g_node, 1, ed_issue, NULL);
        return;
    }
    if (!inline_cb) {
        ed_issue(NULL);
    }
}

static void ed_issue(void *user) {
    (void)user;
    if (g_in_issue) {
        return;
    }
    g_in_issue = 1;
    for (;;) {
        uint64_t limit = g_window ? g_window : 1;
        uint64_t id = 0;
        int rc;
        g_pause = 0;
        if (now_ns() >= g_deadline_ns ||
            (g_opt.max_requests != 0 && g_attempted >= g_opt.max_requests)) {
            if (g_inflight == 0) {
                ed_finish();
            }
            break;
        }
        if (g_inflight >= limit) {
            break; /* 在途额度已满，等响应回收后再补发 */
        }
        g_attempted++;
        g_inflight++;
        if (g_inflight > g_inflight_peak) {
            g_inflight_peak = g_inflight;
        }
        {
            payload_t pl;
            pl.data = g_payload;
            pl.len = g_opt.payload_bytes;
            pl.encoding = 0;
            rc = proxy_call_async(g_proxy, g_opt.service, g_opt.method, &pl, ed_resp,
                                  (void *)(uintptr_t)now_ns(), g_opt.timeout_ms, &id);
        }
        if (rc != OK) {
            g_failed++;
            error_add(g_errors, &g_error_count, rc);
            g_inflight--;
            (void)node_post_delay(g_node, 1, ed_issue, NULL);
            break;
        }
        if (g_pause) {
            break; /* 回调内联执行并要求暂停（NOTFOUND / 已到期） */
        }
    }
    g_in_issue = 0;
}

int main(int argc, char **argv) {
    options_t opt;
    node_config_t cfg;
    node_t *node = NULL;
    proxy_t *proxy = NULL;
    ntk_sem *sem = NULL;
    callout_t co;
    uint8_t *payload = NULL;
    error_entry_t errors[16];
    int error_count = 0;
    uint64_t attempted = 0, succeeded = 0, failed = 0, response_bytes = 0;
    uint64_t peak_inflight = 0;
    uint64_t min_ns = 0, max_ns = 0, sum_ns = 0, lat_count = 0;
    uint64_t start_ns, deadline_ns, end_ns;
    char *colon;
    char peer_host[64];
    char target_host[64];
    int i;
    int rc;
    node_mode_t mode = MODE_MESH;
    FILE *out = stdout;

    memset(&opt, 0, sizeof(opt));
    opt.service = "OrderService";
    opt.method = "get";
    opt.output = "-";
    opt.duration_seconds = 15;
    opt.payload_bytes = 16;
    opt.timeout_ms = 5000;
    opt.connect_timeout_seconds = 20;
    opt.bind = "0.0.0.0";

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--transport") == 0 && i + 1 < argc) {
            const char *t = argv[++i];
            opt.transport = (strcmp(t, "tcp") == 0 || strcmp(t, "TCP") == 0) ? TRANSPORT_TCP
                                                                             : TRANSPORT_UDP;
        } else if (strcmp(a, "--service") == 0 && i + 1 < argc) {
            opt.service = argv[++i];
        } else if (strcmp(a, "--mode") == 0 && i + 1 < argc) {
            mode = (strcmp(argv[++i], "static") == 0) ? MODE_STATIC : MODE_MESH;
        } else if (strcmp(a, "--method") == 0 && i + 1 < argc) {
            opt.method = argv[++i];
        } else if (strcmp(a, "--worker-id") == 0 && i + 1 < argc) {
            opt.worker_id = argv[++i];
        } else if (strcmp(a, "--output") == 0 && i + 1 < argc) {
            opt.output = argv[++i];
        } else if (strcmp(a, "--duration-seconds") == 0 && i + 1 < argc) {
            parse_u64(argv[++i], &opt.duration_seconds);
        } else if (strcmp(a, "--max-requests") == 0 && i + 1 < argc) {
            parse_u64(argv[++i], &opt.max_requests);
        } else if (strcmp(a, "--payload-bytes") == 0 && i + 1 < argc) {
            parse_size(argv[++i], &opt.payload_bytes);
        } else if (strcmp(a, "--expect-response-bytes") == 0 && i + 1 < argc) {
            parse_size(argv[++i], &opt.expect_response_bytes);
        } else if (strcmp(a, "--timeout-ms") == 0 && i + 1 < argc) {
            parse_u64(argv[++i], &opt.timeout_ms);
        } else if (strcmp(a, "--connect-timeout-seconds") == 0 && i + 1 < argc) {
            uint64_t v = 0;
            parse_u64(argv[++i], &v);
            opt.connect_timeout_seconds = (uint32_t)v;
        } else if (strcmp(a, "--rate") == 0 && i + 1 < argc) {
            opt.rate = atof(argv[++i]);
        } else if (strcmp(a, "--window") == 0 && i + 1 < argc) {
            parse_u64(argv[++i], &opt.window);
        } else if (strcmp(a, "--verify-response") == 0) {
            opt.verify_response = 1;
        } else if (strcmp(a, "--discovery-port") == 0 && i + 1 < argc) {
            opt.discovery_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(a, "--data-port") == 0 && i + 1 < argc) {
            opt.data_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(a, "--max-msg-bytes") == 0 && i + 1 < argc) {
            opt.max_msg_bytes = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(a, "--backpressure-bytes") == 0 && i + 1 < argc) {
            opt.backpressure_bytes = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(a, "--frag-bytes") == 0 && i + 1 < argc) {
            opt.frag_bytes = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(a, "--bind") == 0 && i + 1 < argc) {
            opt.bind = argv[++i];
        } else if (strcmp(a, "--peer") == 0 && i + 1 < argc) {
            char *pair = argv[++i];
            colon = strrchr(pair, ':');
            if (colon) {
                size_t hl = (size_t)(colon - pair);
                if (hl >= sizeof(peer_host)) {
                    hl = sizeof(peer_host) - 1;
                }
                memcpy(peer_host, pair, hl);
                peer_host[hl] = '\0';
                opt.peer_ip = peer_host;
                opt.peer_port = (uint16_t)atoi(colon + 1);
            }
        } else if (strcmp(a, "--target") == 0 && i + 1 < argc) {
            char *pair = argv[++i];
            colon = strrchr(pair, ':');
            if (colon) {
                size_t hl = (size_t)(colon - pair);
                if (hl >= sizeof(target_host)) {
                    hl = sizeof(target_host) - 1;
                }
                memcpy(target_host, pair, hl);
                target_host[hl] = '\0';
                opt.target_ip = target_host;
                opt.target_port = (uint16_t)atoi(colon + 1);
            }
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = opt.worker_id ? opt.worker_id : "loadgen";
    cfg.transport = opt.transport;
    cfg.bind_host = opt.bind;
    cfg.data_port = opt.data_port;
    cfg.discovery_host = "255.255.255.255";
    cfg.discovery_port = opt.discovery_port;
    cfg.mode = mode;
    cfg.io_threads = 1;
    cfg.hello_interval_ms = 100;
    cfg.node_ttl_ms = 3000;
    cfg.frag_bytes = opt.frag_bytes;
    cfg.max_msg_bytes = opt.max_msg_bytes;
    cfg.backpressure_bytes = opt.backpressure_bytes;

    sem = ntk_sem_create(0);
    if (!sem) {
        rpc_log_error("sem create failed");
        return 1;
    }
    if (opt.payload_bytes > 0) {
        payload = (uint8_t *)malloc(opt.payload_bytes);
        if (!payload) {
            return 1;
        }
        for (i = 0; i < (int)opt.payload_bytes; i++) {
            payload[i] = (uint8_t)('a' + (i % 26));
        }
    }

    rc = node_create(&cfg, &node);
    if (rc != OK) {
        rpc_log_error("node create failed: %d", rc);
        return 1;
    }
    if (opt.peer_ip) {
        (void)node_add_discovery_peer(node, opt.peer_ip, opt.peer_port);
    }
    rc = proxy_create(node, NULL, &proxy);
    if (rc != OK) {
        rpc_log_error("proxy create failed: %d", rc);
        return 1;
    }
    if (opt.target_ip && opt.target_port) {
        (void)proxy_set_endpoint(proxy, opt.target_ip, opt.target_port,
                                      node_transport(node));
    }

    memset(&co, 0, sizeof(co));
    co.sem = sem;

    /* MESH：先等目标服务进入路由表（启动等待），再做预热。 */
    if (!(opt.target_ip && opt.target_port)) {
        (void)node_wait_for_service(node, opt.service,
                                         (uint64_t)opt.connect_timeout_seconds * 1000ull);
    }

    /* 预热：等发现收敛（NOTFOUND 视为尚未发现，继续重试）。 */
    {
        uint64_t wdeadline = now_ns() + (uint64_t)opt.connect_timeout_seconds * 1000000000ull;
        uint64_t id = 0;
        int warmed = 0;
        while (now_ns() < wdeadline) {
            payload_t pl;
            pl.data = payload;
            pl.len = opt.payload_bytes;
            pl.encoding = 0;
            co.status = -999;
            co.send_ns = now_ns();
            rc = proxy_call_async(proxy, opt.service, opt.method, &pl, on_response, &co,
                                  opt.timeout_ms, &id);
            if (rc != OK) {
                rpc_log_error("proxy_call_async failed: %d", rc);
                node_destroy(node);
                return 1;
            }
            (void)ntk_sem_timedwait(sem, (unsigned)(opt.timeout_ms + 100));
            if (co.status != ERR_NOTFOUND) {
                warmed = 1;
                break;
            }
            ntk_sleep_ms(20);
        }
        if (!warmed) {
            g_status = 1; /* discovery_error */
        }
    }

    memset(g_lat_hist, 0, sizeof(g_lat_hist));
    start_ns = now_ns();
    deadline_ns = start_ns + opt.duration_seconds * 1000000000ull;
    if (opt.verify_response) {
        g_expect_data = payload;
        g_expect_len = opt.payload_bytes;
    }

    if (g_status == 0) {
        if (opt.rate > 0.0) {
            uint64_t index = 0;
            while (now_ns() < deadline_ns &&
                   (opt.max_requests == 0 || attempted < opt.max_requests)) {
                uint64_t id = 0;
                uint64_t us;
                uint64_t target = start_ns + (uint64_t)((double)index * 1000000000.0 / opt.rate);
                payload_t pl;
                sleep_until_ns(target);
                pl.data = payload;
                pl.len = opt.payload_bytes;
                pl.encoding = 0;
                co.status = -999;
                co.resp_len = 0;
                co.verify_ok = 1;
                co.send_ns = now_ns();
                rc = proxy_call_async(proxy, opt.service, opt.method, &pl, on_response,
                                      &co, opt.timeout_ms, &id);
                attempted++;
                if (rc != OK) {
                    failed++;
                    error_add(errors, &error_count, rc);
                    index++;
                    continue;
                }
                if (ntk_sem_timedwait(sem, (unsigned)(opt.timeout_ms + 100)) != NTK_OK) {
                    failed++;
                    error_add(errors, &error_count, ERR_TIMEOUT);
                    index++;
                    continue;
                }
                us = (co.latency_ns + 999ull) / 1000ull;
                {
                    uint64_t hidx = (uint64_t)lat_hist_index(us);
                    g_lat_hist[hidx]++;
                }
                if (co.status == OK && co.app_status == OK && co.verify_ok &&
                    (opt.expect_response_bytes == 0 || co.resp_len == opt.expect_response_bytes)) {
                    succeeded++;
                    response_bytes += co.resp_len;
                    if (lat_count == 0 || co.latency_ns < min_ns) {
                        min_ns = co.latency_ns;
                    }
                    if (co.latency_ns > max_ns) {
                        max_ns = co.latency_ns;
                    }
                    sum_ns += co.latency_ns;
                    lat_count++;
                } else {
                    failed++;
                    error_add(errors, &error_count,
                              co.status != OK ? co.status : co.app_status);
                }
                index++;
            }
        } else {
            /* 事件驱动：全程在 poller 线程内联 ping-pong */
            g_node = node;
            g_proxy = proxy;
            g_opt = opt;
            g_payload = payload;
            g_deadline_ns = deadline_ns;
            g_attempted = g_succeeded = g_failed = g_response_bytes = 0;
            g_min_ns = g_max_ns = g_sum_ns = g_lat_count = 0;
            g_error_count = 0;
            g_window = opt.window;
            g_inflight = 0;
            g_inflight_peak = 0;
            g_finished = 0;
            g_done = ntk_sem_create(0);
            if (g_done) {
                (void)node_post(node, ed_issue, NULL);
                (void)ntk_sem_timedwait(
                    g_done, (unsigned)(opt.duration_seconds * 1000 + opt.timeout_ms + 2000));
                ntk_sem_destroy(g_done);
                g_done = NULL;
            }
            attempted = g_attempted;
            succeeded = g_succeeded;
            failed = g_failed;
            response_bytes = g_response_bytes;
            min_ns = g_min_ns;
            max_ns = g_max_ns;
            sum_ns = g_sum_ns;
            lat_count = g_lat_count;
            memcpy(errors, g_errors, sizeof(errors));
            error_count = g_error_count;
            peak_inflight = g_inflight_peak;
        }
    }
    end_ns = now_ns();

    if (!opt.output || strcmp(opt.output, "-") == 0) {
        out = stdout;
    } else {
        out = fopen(opt.output, "w");
        if (!out) {
            out = stdout;
        }
    }

    {
        double duration = (double)(end_ns - start_ns) / 1000000000.0;
        const char *status = (g_status == 1) ? "discovery_error" : (failed == 0 ? "ok" : "error");
        fprintf(out, "{");
        fprintf(out, "\"status\":\"%s\",", status);
        fprintf(out, "\"worker_id\":\"%s\",", opt.worker_id ? opt.worker_id : "");
        fprintf(out, "\"service\":\"%s\",\"method\":\"%s\",", opt.service, opt.method);
        fprintf(out, "\"attempted\":%llu,\"succeeded\":%llu,\"failed\":%llu,",
                (unsigned long long)attempted, (unsigned long long)succeeded,
                (unsigned long long)failed);
        fprintf(out, "\"peak_inflight\":%llu,", (unsigned long long)peak_inflight);
        fprintf(out, "\"response_bytes\":%llu,", (unsigned long long)response_bytes);
        fprintf(out, "\"expected_response_bytes\":%llu,", (unsigned long long)opt.expect_response_bytes);
        fprintf(out, "\"duration_seconds\":%.6f,", duration);
        fprintf(out, "\"errors\":{");
        for (i = 0; i < error_count; i++) {
            fprintf(out, "%s\"%d\":%llu", i ? "," : "", errors[i].code,
                    (unsigned long long)errors[i].count);
        }
        fprintf(out, "},");
        fprintf(out, "\"latency_us\":{\"min\":%.1f,\"mean\":%.1f,\"max\":%.1f},",
                lat_count ? (double)min_ns / 1000.0 : 0.0,
                lat_count ? (double)sum_ns / (double)lat_count / 1000.0 : 0.0,
                lat_count ? (double)max_ns / 1000.0 : 0.0);
        fprintf(out, "\"latency_histogram_us\":{");
        {
            int first = 1;
            for (i = 0; i < LAT_HIST_SIZE; i++) {
                if (g_lat_hist[i]) {
                    fprintf(out, "%s\"%llu\":%llu", first ? "" : ",",
                            (unsigned long long)lat_hist_key(i),
                            (unsigned long long)g_lat_hist[i]);
                    first = 0;
                }
            }
        }
        {
            metrics_t metrics;
            memset(&metrics, 0, sizeof(metrics));
            if (node) {
                node_metrics(node, &metrics);
            }
            fprintf(out,
                    "},\"metrics\":{\"requests_sent\":%llu,\"responses_received\":%llu,"
                    "\"requests_failed\":%llu,\"timeouts\":%llu,\"retries\":%llu,"
                    "\"bytes_sent\":%llu,\"bytes_received\":%llu,"
                    "\"udp_packets_sent\":%llu,\"udp_packets_received\":%llu,"
                    "\"udp_retransmissions\":%llu,\"udp_nacks_sent\":%llu,"
                    "\"udp_reassembly_expired\":%llu,\"tcp_frames_sent\":%llu,"
                    "\"tcp_frames_received\":%llu,"
                    "\"rtcp_sr_sent\":%llu,\"rtcp_rr_sent\":%llu,"
                    "\"rtcp_sr_received\":%llu,\"rtcp_rr_received\":%llu,"
                    "\"rtcp_rtt_ms\":%u,\"rtcp_jitter_ms\":%u,\"rtcp_remote_lost\":%d}}\n",
                    (unsigned long long)metrics.requests_sent,
                    (unsigned long long)metrics.responses_received,
                    (unsigned long long)metrics.requests_failed,
                    (unsigned long long)metrics.timeouts,
                    (unsigned long long)metrics.retries,
                    (unsigned long long)metrics.bytes_sent,
                    (unsigned long long)metrics.bytes_received,
                    (unsigned long long)metrics.udp_packets_sent,
                    (unsigned long long)metrics.udp_packets_received,
                    (unsigned long long)metrics.udp_retransmissions,
                    (unsigned long long)metrics.udp_nacks_sent,
                    (unsigned long long)metrics.udp_reassembly_expired,
                    (unsigned long long)metrics.tcp_frames_sent,
                    (unsigned long long)metrics.tcp_frames_received,
                    (unsigned long long)metrics.rtcp_sr_sent,
                    (unsigned long long)metrics.rtcp_rr_sent,
                    (unsigned long long)metrics.rtcp_sr_received,
                    (unsigned long long)metrics.rtcp_rr_received,
                    (unsigned)metrics.rtcp_rtt_ms,
                    (unsigned)metrics.rtcp_jitter_ms,
                    (int)metrics.rtcp_remote_lost);
        }
    }

    if (out != stdout) {
        fclose(out);
    }
    free(payload);
    ntk_sem_destroy(sem);
    proxy_destroy(proxy);
    node_destroy(node);
    return 0;
}
