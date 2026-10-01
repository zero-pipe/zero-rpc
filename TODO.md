# zrpc 待办（下次继续）

## 测试体系
- [x] Linux 干净构建通过 unit/integration（含 GTest 模块测试）。
- [x] Linux perf smoke 与 stress 场景通过，初始性能基线已生成。
- [x] `analyze` 阶段把 soak 的 `resource`（RSS 增长 / fd 增长）纳入判定，输出 `leak_suspected` 并影响退出码。
- [x] `zbench.py sanitizer` 已实跑验证（ASan/UBSan 跑 unit+integration）。
- [x] 跑通整条门禁：`scripts/check.sh`（unit + perf smoke + stress + 基线回归）端到端验证。
- [x] 在 Linux 上重采样完整矩阵基线，覆盖 UDP/TCP payload、并发和 10k QPS 场景。
- [x] 把 valgrind（memcheck + `--track-fds=yes`）作为一个 tier 集成进 `zbench.py`。
- [ ] 决定是否把 `3rdpart/zero-tool-kit/tests` 纳入顶层 CTest（当前顶层强制 `ZTK_BUILD_TESTS=OFF`）。
- [x] Windows 上跑通 `zbench.py unit` / `perf --quick`（资源采样在 Windows 自动跳过）。

## 已知性能/正确性
- [ ] UDP 会话表容量与淘汰：单 IO 64 槽，`N=128` 并发仍会因 REUSEPORT 分布不均而颠簸；评估更优的分配/淘汰策略。
- [ ] TCP 大包已从 2.5s@32MiB 优化到 ~0.1s 且校验正确；继续验证 >32MiB / 极端场景的边界。

## 仓库
- [ ] `3rdpart/zero-tool-kit/.git` 已移除（vendored），如需保留独立历史请改回 submodule 并推送其远端。
- [ ] 首次 `git push` 到 https://github.com/zero-pipe/zero-rpc.git 需要凭据；确认远端是否已有内容需先 pull/合并。
