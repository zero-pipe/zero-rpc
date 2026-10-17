# Google Benchmark Test Dependency

`include/` contains the vendored Google Benchmark headers. `lib/<os>_<arch>/`
contains the matching prebuilt static archives used only when
`FC_BUILD_PERF=ON`.

The performance CMake configuration resolves the platform archive from this
directory. Linux amd64 currently ships `libbenchmark.a`. When `FC_BUILD_PERF=ON`
but the archive for `${OS}_${ARCH}` is missing (Windows, Linux arm64, and
others), CMake warns and skips performance targets instead of failing, so
GoogleTest builds are unaffected. Production libraries and examples do not
link Google Benchmark.

Benchmark sources live under `tests/perf/<module>/` and are registered with
`fc_benchmark` from `cmake/tests/perf/*.cmake`. Cases that already provide
`BENCHMARK_MAIN()` should link only `libbenchmark.a`, not
`libbenchmark_main.a`.

Bundled archive directories:

- `linux_amd64`

This migration verified the `linux_amd64` archives with GCC 8.5. Other
platforms require their corresponding compiler and runtime validation before
being treated as supported CI targets.
