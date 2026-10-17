/*
 * Synchronous rpc benchmark client.
 *
 * One call is outstanding per process. Each iteration uses
 * proxy_call_sync(), so the reported latency includes the blocking
 * wrapper and response-buffer copy/release path.
 */
#include <client.h>

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

#define LAT_LINEAR_MAX 65536
#define LAT_LOG_BUCKETS 24
#define LAT_HIST_SIZE (LAT_LINEAR_MAX + LAT_LOG_BUCKETS)

typedef struct options {
    const char *target;
    const char *service;
    const char *method;
    const char *worker_id;
    const char *output;
    uint64_t duration_seconds;
    uint64_t max_requests;
    uint64_t timeout_ms;
    size_t payload_bytes;
    size_t expect_response_bytes;
    int verify_response;
    transport_kind_t transport;
} options_t;

static int parse_u64(const char *value, uint64_t *out) {
    char *end = NULL;
    unsigned long long parsed;
    if (!value || !value[0]) {
        return 0;
    }
    parsed = strtoull(value, &end, 10);
    if (!end || *end != '\0') {
        return 0;
    }
    *out = (uint64_t)parsed;
    return 1;
}

static int parse_size(const char *value, size_t *out) {
    uint64_t parsed;
    if (!parse_u64(value, &parsed)) {
        return 0;
    }
    *out = (size_t)parsed;
    return 1;
}

static int lat_hist_index(uint64_t us) {
    int bucket = 0;
    if (us < LAT_LINEAR_MAX) {
        return (int)us;
    }
    while ((us >> (bucket + 1)) != 0) {
        bucket++;
    }
    bucket -= 16;
    if (bucket >= LAT_LOG_BUCKETS) {
        bucket = LAT_LOG_BUCKETS - 1;
    }
    return LAT_LINEAR_MAX + bucket;
}

static uint64_t lat_hist_key(int index) {
    uint64_t lower;
    uint64_t upper;
    if (index < LAT_LINEAR_MAX) {
        return (uint64_t)index;
    }
    lower = (uint64_t)1 << (16 + index - LAT_LINEAR_MAX);
    upper = (lower << 1) - 1;
    return lower + (upper - lower) / 2;
}

static uint64_t histogram_percentile(const uint64_t *hist, uint64_t count, double p) {
    uint64_t rank;
    uint64_t seen = 0;
    int i;
    if (count == 0) {
        return 0;
    }
    rank = (uint64_t)((double)(count - 1) * p) + 1;
    for (i = 0; i < LAT_HIST_SIZE; i++) {
        seen += hist[i];
        if (seen >= rank) {
            return lat_hist_key(i);
        }
    }
    return lat_hist_key(LAT_HIST_SIZE - 1);
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s --target host:port [--transport tcp|udp] [--service s] [--method m]\n"
            "          [--duration-seconds n] [--max-requests n] [--payload-bytes n]\n"
            "          [--expect-response-bytes n] [--timeout-ms n] [--verify-response]\n"
            "          [--worker-id id] [--output path|-]\n",
            argv0);
}

int main(int argc, char **argv) {
    options_t opt;
    node_config_t cfg;
    node_t *node = NULL;
    proxy_t *proxy = NULL;
    payload_t request;
    uint8_t *payload = NULL;
    uint64_t hist[LAT_HIST_SIZE];
    uint64_t attempted = 0;
    uint64_t succeeded = 0;
    uint64_t failed = 0;
    uint64_t response_bytes = 0;
    uint64_t min_ns = 0;
    uint64_t max_ns = 0;
    uint64_t sum_ns = 0;
    uint64_t start_ns;
    uint64_t deadline_ns;
    uint64_t end_ns;
    int errors[16];
    uint64_t error_counts[16];
    int error_count = 0;
    FILE *out = stdout;
    int i;
    int rc = OK;

    memset(&opt, 0, sizeof(opt));
    memset(hist, 0, sizeof(hist));
    memset(errors, 0, sizeof(errors));
    memset(error_counts, 0, sizeof(error_counts));
    opt.target = "127.0.0.1:42410";
    opt.service = "OrderService";
    opt.method = "get";
    opt.output = "-";
    opt.duration_seconds = 15;
    opt.payload_bytes = 64;
    opt.timeout_ms = 5000;
    opt.transport = TRANSPORT_TCP;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--target") == 0 && i + 1 < argc) {
            opt.target = argv[++i];
        } else if (strcmp(arg, "--transport") == 0 && i + 1 < argc) {
            const char *transport = argv[++i];
            opt.transport = (strcmp(transport, "udp") == 0 || strcmp(transport, "UDP") == 0)
                                ? TRANSPORT_UDP
                                : TRANSPORT_TCP;
        } else if (strcmp(arg, "--service") == 0 && i + 1 < argc) {
            opt.service = argv[++i];
        } else if (strcmp(arg, "--method") == 0 && i + 1 < argc) {
            opt.method = argv[++i];
        } else if (strcmp(arg, "--worker-id") == 0 && i + 1 < argc) {
            opt.worker_id = argv[++i];
        } else if (strcmp(arg, "--output") == 0 && i + 1 < argc) {
            opt.output = argv[++i];
        } else if (strcmp(arg, "--duration-seconds") == 0 && i + 1 < argc) {
            parse_u64(argv[++i], &opt.duration_seconds);
        } else if (strcmp(arg, "--max-requests") == 0 && i + 1 < argc) {
            parse_u64(argv[++i], &opt.max_requests);
        } else if (strcmp(arg, "--timeout-ms") == 0 && i + 1 < argc) {
            parse_u64(argv[++i], &opt.timeout_ms);
        } else if (strcmp(arg, "--payload-bytes") == 0 && i + 1 < argc) {
            parse_size(argv[++i], &opt.payload_bytes);
        } else if (strcmp(arg, "--expect-response-bytes") == 0 && i + 1 < argc) {
            parse_size(argv[++i], &opt.expect_response_bytes);
        } else if (strcmp(arg, "--verify-response") == 0) {
            opt.verify_response = 1;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (opt.payload_bytes > 0) {
        payload = (uint8_t *)malloc(opt.payload_bytes);
        if (!payload) {
            fprintf(stderr, "payload allocation failed\n");
            return 1;
        }
        for (i = 0; i < (int)opt.payload_bytes; i++) {
            payload[i] = (uint8_t)('a' + (i % 26));
        }
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = opt.worker_id ? opt.worker_id : "sync_loadgen";
    cfg.transport = opt.transport;
    cfg.bind_host = "0.0.0.0";
    cfg.mode = MODE_STATIC;
    cfg.io_threads = 1;
    rc = node_create(&cfg, &node);
    if (rc == OK) {
        rc = proxy_create(node, NULL, &proxy);
    }
    if (rc == OK) {
        char endpoint[128];
        snprintf(endpoint, sizeof(endpoint), "%s://%s",
                 opt.transport == TRANSPORT_TCP ? "tcp" : "udp", opt.target);
        rc = proxy_set_endpoint_uri(proxy, endpoint);
    }
    if (rc != OK) {
        fprintf(stderr, "client setup failed: %d\n", rc);
        free(payload);
        proxy_destroy(proxy);
        node_destroy(node);
        return 1;
    }

    request.data = payload;
    request.len = opt.payload_bytes;
    request.encoding = 0;

    /* Exclude connection establishment and one-time route setup from results. */
    {
        response_t warm_response;
        memset(&warm_response, 0, sizeof(warm_response));
        rc = proxy_call_sync(proxy, opt.service, opt.method, &request,
                                  &warm_response, opt.timeout_ms, NULL);
        response_release(&warm_response);
        if (rc != OK) {
            fprintf(stderr, "warmup call failed: %d\n", rc);
            free(payload);
            proxy_destroy(proxy);
            node_destroy(node);
            return 1;
        }
    }

    start_ns = now_ns();
    deadline_ns = start_ns + opt.duration_seconds * 1000000000ull;
    while (now_ns() < deadline_ns &&
           (opt.max_requests == 0 || attempted < opt.max_requests)) {
        response_t response;
        uint64_t call_start = now_ns();
        uint64_t latency;
        uint64_t us;
        int call_ok;

        memset(&response, 0, sizeof(response));
        rc = proxy_call_sync(proxy, opt.service, opt.method, &request,
                                  &response, opt.timeout_ms, NULL);
        latency = now_ns() - call_start;
        attempted++;
        us = (latency + 999ull) / 1000ull;
        hist[lat_hist_index(us)]++;
        call_ok = (rc == OK &&
                   (opt.expect_response_bytes == 0 ||
                    response.payload.len == opt.expect_response_bytes));
        if (call_ok && opt.verify_response) {
            call_ok = response.payload.len == opt.payload_bytes &&
                      memcmp(response.payload.data, payload, opt.payload_bytes) == 0;
        }
        if (call_ok) {
            succeeded++;
            response_bytes += response.payload.len;
            if (succeeded == 1 || latency < min_ns) {
                min_ns = latency;
            }
            if (latency > max_ns) {
                max_ns = latency;
            }
            sum_ns += latency;
        } else {
            int error = rc;
            int found = 0;
            int error_index;
            failed++;
            if (rc == OK) {
                error = ERR_IO;
            }
            for (error_index = 0; error_index < error_count; error_index++) {
                if (errors[error_index] == error) {
                    error_counts[error_index]++;
                    found = 1;
                    break;
                }
            }
            if (!found && error_count < 16) {
                errors[error_count] = error;
                error_counts[error_count++] = 1;
            }
        }
        response_release(&response);
    }
    end_ns = now_ns();

    if (opt.output && strcmp(opt.output, "-") != 0) {
        out = fopen(opt.output, "w");
        if (!out) {
            out = stdout;
        }
    }
    {
        double duration = (double)(end_ns - start_ns) / 1000000000.0;
        fprintf(out, "{\"status\":\"%s\",\"worker_id\":\"%s\","
                     "\"attempted\":%llu,\"succeeded\":%llu,\"failed\":%llu,"
                     "\"response_bytes\":%llu,\"duration_seconds\":%.6f,\"qps\":%.3f,",
                failed == 0 ? "ok" : "error", opt.worker_id ? opt.worker_id : "",
                (unsigned long long)attempted, (unsigned long long)succeeded,
                (unsigned long long)failed, (unsigned long long)response_bytes,
                duration, duration > 0.0 ? (double)succeeded / duration : 0.0);
        fprintf(out, "\"errors\":{");
        for (i = 0; i < error_count; i++) {
            fprintf(out, "%s\"%d\":%llu", i ? "," : "", errors[i],
                    (unsigned long long)error_counts[i]);
        }
        fprintf(out, "},\"latency_us\":{\"min\":%.1f,\"mean\":%.1f,"
                     "\"p50\":%llu.0,\"p95\":%llu.0,\"p99\":%llu.0,\"max\":%.1f},"
                     "\"latency_histogram_us\":{",
                succeeded ? (double)min_ns / 1000.0 : 0.0,
                succeeded ? (double)sum_ns / (double)succeeded / 1000.0 : 0.0,
                (unsigned long long)histogram_percentile(hist, succeeded, 0.50),
                (unsigned long long)histogram_percentile(hist, succeeded, 0.95),
                (unsigned long long)histogram_percentile(hist, succeeded, 0.99),
                succeeded ? (double)max_ns / 1000.0 : 0.0);
        {
            int first = 1;
            for (i = 0; i < LAT_HIST_SIZE; i++) {
                if (hist[i]) {
                    fprintf(out, "%s\"%llu\":%llu", first ? "" : ",",
                            (unsigned long long)lat_hist_key(i),
                            (unsigned long long)hist[i]);
                    first = 0;
                }
            }
        }
        fprintf(out, "},\"metrics\":{}}\n");
    }
    if (out != stdout) {
        fclose(out);
    }
    free(payload);
    proxy_destroy(proxy);
    node_destroy(node);
    return failed == 0 ? 0 : 2;
}
