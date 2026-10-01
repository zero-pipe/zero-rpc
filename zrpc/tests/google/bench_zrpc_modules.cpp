#include <benchmark/benchmark.h>

extern "C" {
#include "zrpc_envelope.h"
}

static void BM_EnvelopeEncode64(benchmark::State &state) {
    char payload[64] = {};
    char route[] = "OrderService.get";
    uint8_t buffer[256] = {};
    zrpc_envelope_t envelope = {};
    envelope.kind = ZRPC_KIND_REQUEST;
    envelope.request_id = 1;
    envelope.route = route;
    envelope.payload = payload;
    envelope.payload_len = sizeof(payload);
    for (auto _ : state) {
        benchmark::DoNotOptimize(zrpc_envelope_encode(buffer, sizeof(buffer), &envelope));
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * sizeof(payload)));
}

static void BM_EnvelopeEncode32K(benchmark::State &state) {
    static char payload[32 * 1024] = {};
    char route[] = "OrderService.get";
    static uint8_t buffer[33 * 1024] = {};
    zrpc_envelope_t envelope = {};
    envelope.kind = ZRPC_KIND_REQUEST;
    envelope.request_id = 1;
    envelope.route = route;
    envelope.payload = payload;
    envelope.payload_len = sizeof(payload);
    for (auto _ : state) {
        benchmark::DoNotOptimize(zrpc_envelope_encode(buffer, sizeof(buffer), &envelope));
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * sizeof(payload)));
}

BENCHMARK(BM_EnvelopeEncode64);
BENCHMARK(BM_EnvelopeEncode32K);
BENCHMARK_MAIN();
