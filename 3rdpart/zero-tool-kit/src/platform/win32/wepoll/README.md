# wepoll（vendored）

Windows 上的 epoll API 实现，源码 vendored 进 ZTK。

| 文件 | 说明 |
|------|------|
| `wepoll.c` / `wepoll.h` | 上游 [piscisaureus/wepoll](https://github.com/piscisaureus/wepoll) |
| `sys/epoll.h` | ZTK 适配头（屏蔽 `EPOLLET`） |
| `LICENSE` | BSD 许可证 |

被 `src/poller/win32/poller_wepoll.c` 使用；CMake 通过 `src/platform/win32/wepoll` 与 `sys/` 加入 include 路径。
