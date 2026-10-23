# Cyclone DDS (vendored SDK)

Prebuilt Cyclone DDS runtime used by the `cyclone.dds` transport driver.

- Version: Cyclone DDS **0.11.0**
- License: Eclipse Public License 2.0 / Eclipse Distribution License 1.0
  (see the headers under `<os>_<arch>/include/dds` for SPDX identifiers)
- Origin: relocated from the fastcall 2.0 tree. Public headers come from
  `components/dds/cyclonedds/third/<os>_<arch>/include`; the Windows runtime
  (`ddsc.lib` / `ddsc.dll`) is the `deplibs/windows_amd64` build that the
  previous adapter linked (the `third` Windows DLL rejects the XTypes metadata
  used by the generated topic descriptor). Linux `libddsc.so` is identical in
  both trees. No Cyclone source is compiled here.

## Layout

```
<os>_<arch>/
  include/   # public headers; add .../include to the compiler search path
  lib/       # ddsc.lib (Windows import lib) or libddsc.so (Linux)
  bin/       # ddsc.dll (Windows only)
```

Supported trees: `windows_amd64`, `linux_amd64`, `linux_arm64`,
`linux_loongarch64`.

`linux_loongarch64` was relocated from the fastcall 2.0 tree
(`components/dds/cyclonedds/third/linux_loongarch64`). Its public headers match
the other Linux trees byte for byte apart from one added NOLINT comment in
`dds/export.h`, and the runtime was validated end to end on loongarch64
(request/response over the `cyclone.dds` transport).

## Build wiring

`cmake/third_party/cyclonedds.cmake` selects the tree for the active
`OS_NAME`/`ARCH_NAME` and exposes plain path variables
(`FC_CYCLONEDDS_ROOT` / `FC_CYCLONEDDS_INCLUDE` / `FC_CYCLONEDDS_LIB` /
`FC_CYCLONEDDS_DLL`) rather than a target. It is included only when
`FC_BUILD_DDS` is set, which is derived from the `FC_ENABLE_DDS` CMake option
(default ON, and automatically disabled on platforms without a vendored SDK).

On Windows the driver's post-build step copies `ddsc.dll` next to the runtime
outputs so executables can load it at launch.

## IDL payload type

The transport carries already-encoded framework frames as opaque octet
samples. The generated C type lives with the driver at
`src/framework/transport/drivers/dds/idl/` (`cy_dds_message.idl` and the
generated `.h`/`.c`). Regenerate with the Cyclone DDS 0.11.0 `idlc` tool; do not
hand-edit the generated files.
