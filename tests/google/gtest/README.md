# GoogleTest Test Dependency

`include/` contains the vendored GoogleTest headers. `lib/<os>_<arch>/`
contains the matching prebuilt GoogleTest and GoogleMock static archives used
only when `BUILD_TESTS=ON`.

The test CMake configuration resolves `gtest` and `gtest_main` exclusively
from this directory and fails during configuration when the selected platform
archive is unavailable. Production libraries and examples do not link GTest.

Pass `GMOCK` to `fc_gtest` for tests that use GoogleMock; this links `gmock`
and `gmock_main` from the same platform directory.

Bundled archive directories:

- `linux_amd64`
- `linux_arm64`
- `windows_amd64`

The `windows_amd64` archives are static debug (`/MTd`). MSVC test targets
must be built with `--config Debug`. A Release (`/MT`) test build will fail
to link these libraries; that does not affect production libraries, which
never link GTest.

This migration verified the `linux_amd64` archives with GCC 8.5 and the
`windows_amd64` archives with MSVC 19.29 (Visual Studio 2019). Linux/arm64
test builds require their corresponding compiler and runtime validation
before being treated as supported CI targets.
