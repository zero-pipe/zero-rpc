# zrpc 测试体系（unit / perf / stress / stability）

单一入口，任何改动跑一遍即可把住关。C 侧只提供压测引擎
(`zrpc_bench_server` + `zrpc_loadgen`)，所有编排、采样、报告、门禁都在
`tools/perf/zbench.py`（Python 3，无第三方依赖；Linux 为主，Windows 可跑
unit 与 perf）。

## 快速开始

```sh
# 单元 + 集成（CTest，硬门禁）
python3 tools/perf/zbench.py unit

# 正常性能矩阵（payload × transport × 并发 × 定速）
python3 tools/perf/zbench.py perf --quick

# 异常/弱网场景（丢包、背压、超限、服务不存在、连接失败、中途断开、畸形帧、超时）
python3 tools/perf/zbench.py stress

# 稳定性 soak：长时间 + RSS/fd/线程采样 + 泄漏初筛
python3 tools/perf/zbench.py soak --duration 600 --clients 8

# Linux 内存 / fd 检查（需要安装 valgrind）
python3 tools/perf/zbench.py valgrind

# 读结果、出报告并按基线判回归
python3 tools/perf/zbench.py analyze --run artifacts/<ts> --baseline tools/perf/baseline.json

# 一条命令门禁（build + unit/integration + perf smoke + stress）
scripts/check.sh
# 追加稳定性 soak
scripts/check.sh --full
```

## 分层与门禁

| 层 | 位置 | 门禁 |
| --- | --- | --- |
| unit | `zrpc/tests/`（label `unit`）、ztk tests | 硬门禁，必须全过 |
| integration | `zrpc/tests/`（label `integration`） | 硬门禁，必须全过 |
| perf | `zbench.py perf` | 出报告；有 baseline 时按 QPS/P99 容差判回归 |
| stress | `zbench.py stress` | 场景级 `expect` 判定，必须全过 |
| stability | `zbench.py soak` | 出报告 + 泄漏初筛 |
| sanitizer | `zbench.py sanitizer` | ASan/UBSan 跑 unit/integration |
| memcheck | `zbench.py valgrind` | Linux Valgrind Memcheck + leak/fd 检查 |

## 场景说明

**perf（正常）**：`udp/tcp × {64b,8k,32k,1m,32m}`、`conc{4,16,32}`、`1k-10kqps` 定速。
1 进程 = 1 热连接、1 outstanding RPC（对齐 gRPC 单 channel unary）。

**stress（异常）**：

| 场景 | 注入 | 期望 |
| --- | --- | --- |
| `loss10-8k` / `loss20-1k` | UDP 丢包 10%/20% | NACK/RTX 恢复，无失败 |
| `bp-1m` | 服务端出站高水位 64 KiB | 背压后仍全成功 |
| `oversize` | 客户端 payload > 服务端 `max_msg` | 客户端失败、**服务端存活** |
| `notfound` | 调用未注册服务 | 客户端收到错误、服务端存活 |
| `connect-fail` | 连未监听端口 | 客户端优雅失败 |
| `server-abort` | 大传输中途 kill 服务端 | 客户端不挂起、无崩溃 |
| `malformed` | 向数据端口打随机/畸形帧 | **服务端存活且后续正常请求成功** |
| `timeout` | 服务端 `--delay-ms` 大于客户端超时 | 客户端超时、服务端存活 |

**soak（稳定性）**：定速长跑，采样服务端 RSS / fd / 线程数；`rss_growth`
与 fd 增长写入 summary，超过阈值在分析阶段提示 `leak_suspected`。

## 结果与报告

每次运行写入 `artifacts/<UTC时间戳>/`：

```
artifacts/<ts>/
├── briefing.json          # 全场景聚合（机器可读）
├── report.html            # 人读报告
├── junit.xml              # CI 消费
├── baseline.json          # 本次结果，可作为下一次 --baseline
└── <scenario>/
    ├── summary.json       # 该场景聚合
    ├── worker_N.json      # 每个连接的结果
    ├── server.log / worker_N.log
```

`analyze` 读取 `briefing.json`/`summary.json`，重建报告并按
`--qps-tol`（默认 10%）/ `--p99-tol`（默认 25%）与 baseline 比较，
有回归即非零退出。

`scripts/check.sh` 的 smoke perf 默认使用 25% QPS / 75% P99 容差，
用于避免共享 Linux 主机的短跑调度噪声误报；可用 `ZBENCH_QPS_TOL` 和
`ZBENCH_P99_TOL` 覆盖。专用性能机或发布前检查应直接使用 `zbench.py`
并保持默认的严格容差。

## 基线工作流

1. 在稳定版本上跑完整的 `zbench.py perf`，得到 `artifacts/<ts>/baseline.json`。
2. 把它提升为 `tools/perf/baseline.json`（或指定路径）。
3. 之后每次 `perf`/`check.sh` 传 `--baseline tools/perf/baseline.json` 判回归。

## 备注

- 端口从 44000 起按场景递增分配，避免与常见服务冲突。
- `--quick` 缩短时长并裁剪场景，用于本地迭代；`--full`/`soak` 用于提交前。
- Windows 上资源采样（`/proc`）自动跳过，unit/perf 照常。
- Valgrind tier 只在 Linux 执行；Windows 使用 sanitizer 和 CTest。
