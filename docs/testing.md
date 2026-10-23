# 测试与门禁

仓库只有一套门禁入口：`tools/gate.py`。CTest 负责进程内测试，`gate.py`
负责进程级 e2e、性能和稳定性场景，并把结果统一写入 `artifacts/gate/`。

## 分类

| 门禁 | CTest label | 覆盖内容 | 运行方式 |
| --- | --- | --- | --- |
| unit | `unit` | 纯模块、编解码、配置、分帧、指标、注册表 | `python3 tools/gate.py unit` |
| e2e | `e2e` | 进程内 loopback/stream/api/worker/semantics + 独立 server/client 回显 | `python3 tools/gate.py e2e` |
| perf | 无 | TCP/UDP/DDS 吞吐与延迟冒烟，可选 baseline 回归检查 | `python3 tools/gate.py perf` |
| stability | 无 | 服务端常驻，多波次变 payload 浸泡，检查失败和资源增长 | `python3 tools/gate.py stability` |
| fuzz | `fuzz` | libFuzzer smoke（仅 `BUILD_FUZZERS=ON`） | `python3 tools/gate.py fuzz` |

命名约定：

- CTest 名称统一为 `<category>.<case>`，例如 `unit.framing`、`e2e.loopback`。
- CTest label 同时包含类别和组件，例如 `unit;framing`、`e2e;worker`。
- 不再使用 `integration` 作为顶级分类；端到端行为统一归入 `e2e`。
- C 测试可执行文件继续使用 `test_*`，压测工具使用 `bench_*` / `loadgen`。
- 门禁入口只保留 `tools/gate.py`，不新增零散 shell 脚本。

## 快速开始

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON -DBUILD_TOOLS=ON
python3 tools/gate.py unit
python3 tools/gate.py e2e --transports tcp,udp,dds
python3 tools/gate.py perf --perf-duration 10
python3 tools/gate.py stability --transports tcp --stability-duration 60
```

`gate.py` 默认先执行 `cmake --build`；已经构建过时加 `--no-build`。
Windows 多配置生成器需要指定配置：

```powershell
python tools\gate.py unit --config Debug
python tools\gate.py e2e --config Debug --transports tcp,udp,dds
```

也可以用 CMake 目标（会先构建对应测试/工具，再运行门禁）：

```bash
cmake --build build --target gate-unit
cmake --build build --target gate-e2e
cmake --build build --target gate-perf
cmake --build build --target gate-stability
cmake --build build --target gate-all
```

一次跑完默认门禁：

```bash
python3 tools/gate.py all
```

`all` 默认使用较短时长：perf 5 秒/传输、stability 30 秒（仅 TCP）。
需要完整稳定性矩阵时显式指定：

```bash
python3 tools/gate.py stability --transports tcp,udp,dds --stability-duration 300
```

## 判定标准

### unit

- CTest label `unit` 至少有一个用例。
- 所有用例退出码为 0。
- 没有用例被静默跳过；`BUILD_TESTS=OFF` 会直接失败并提示重新配置。

### e2e

- CTest label `e2e` 全部通过。
- 每个传输至少完成 `--e2e-requests` 个请求（默认 200）。
- `status=ok`、`failed=0`、响应字节数等于成功请求数 × 期望响应大小。
- 服务端在场景期间没有提前退出。
- DDS 在 `NB_ENABLE_DDS=OFF` 时自动跳过，不算失败。

### perf

- 每个传输 `status=ok`、`failed=0`、QPS > 0。
- 默认只记录结果，不做硬件相关阈值判断。
- 传入 `--baseline` 后检查 QPS 和 p99：

```bash
# 首次生成基线
python3 tools/gate.py perf --update-baseline --baseline tests/perf/baseline.json

# 后续检查，允许 20% 波动
python3 tools/gate.py perf --baseline tests/perf/baseline.json --tolerance 0.20
```

基线是机器相关的，建议只提交固定 CI/压测机的基线，或把基线放在
`artifacts/` 下按机器保存。

### stability

- 服务端全程常驻，波次之间不重启。
- 每个波次都 `status=ok`、`failed=0`，请求数大于 0。
- 服务端没有提前崩溃。
- Linux 下额外检查资源峰值增长（默认值）：
  - RSS peak growth ≤ 64 MiB
  - 线程数增长 ≤ 8
  - FD peak growth ≤ 32
- Windows 当前跳过 `/proc` 资源检查，只检查失败数和进程存活。

阈值可按需覆盖：

```bash
python3 tools/gate.py stability \
  --max-rss-growth-mb 128 --max-thread-growth 16 --max-fd-growth 64
```

### fuzz

- 仅在 `BUILD_FUZZERS=ON` 且编译器为 Clang 时存在 `fuzz` 测试。
- CTest label `fuzz` 全部通过；没有 fuzz 测试时门禁显示 `SKIP`。

## 产物

每次运行写入独立目录：

```text
artifacts/gate/<UTC timestamp>/
  summary.json              总结果（status、每个门禁的 status/detail/metrics）
  unit/
    list.log                ctest -N -L unit 的输出
    ctest.log               ctest -L unit 的完整输出
  e2e/
    summary.json
    ctest/ctest.log
    tcp/{server.log,client.log,result.json,server.exit}
    udp/...
    dds/...
  perf/
    summary.json
    metrics.json            每个传输的 QPS / p50 / p90 / p99
    tcp/{server.log,client.log,result.json}
  stability/
    tcp/summary.json
    tcp/wave_0_64b/...
    tcp/wave_1_1360b/...
```

`summary.json` 的 `status` 只有 `pass` / `fail`；子门禁的 `SKIP`
不会让总结果失败。失败时脚本退出码为 1，CI 直接使用该退出码。

## 直接使用 CTest

```bash
cd build
ctest -N -L unit          # 列出 unit 用例
ctest -L unit             # 只跑 unit
ctest -L e2e              # 只跑进程内 e2e
ctest -R unit.framing     # 只跑一个用例
```

Windows 多配置：

```powershell
cd build
ctest -C Debug -L unit
ctest -C Debug -L e2e
```

## 新增测试

单元测试：

1. 新增 `tests/test_foo.c`，沿用现有 `main()` + 返回码风格。
2. 在 `tests/CMakeLists.txt` 添加 `add_test(NAME unit.foo COMMAND test_foo)`。
3. 设置 `LABELS "unit;foo"` 和合理 `TIMEOUT`。

进程内 e2e：

1. 新增 `tests/test_foo_e2e.c`。
2. 测试名使用 `e2e.foo`，label 使用 `e2e;foo`。
3. 需要真实 socket 时使用 `127.0.0.1` 和独立端口，结束时释放节点。

进程级 e2e / perf / stability：

- 统一在 `tools/gate.py` 中增加 case 函数，复用 `server_command`、
  `loadgen_command`、`run_scenario` 和 `evaluate_result`。
- 不要新增独立 shell 脚本；否则产物路径和失败判定会再次分散。

## CI

`.github/workflows/ci.yml` 按门禁拆分步骤：

- `unit`：Debug/Release 矩阵 + sanitizer。
- `e2e`：Release 构建后跑 `gate.py e2e`。
- `perf`：短时 `gate.py perf --perf-duration 5`，只做冒烟。
- `stability`：短时 `gate.py stability --stability-duration 20`。
- `fuzz`：Clang + `BUILD_FUZZERS=ON`，跑 `ctest -L fuzz`。

CI 的性能数据不作为跨机器硬阈值，只保证链路可跑、无失败、无崩溃。
