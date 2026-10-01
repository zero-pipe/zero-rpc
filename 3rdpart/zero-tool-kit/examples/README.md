# Examples

Minimal demos showing common ZTK patterns. Build with `-DZTK_BUILD_EXAMPLES=ON` (default when poller backend is available).

| Demo | Topic |
|------|-------|
| `demo_01_rtsp_manual_poll` | RTSP + `ztk_poller_pool`（size=1） |
| `demo_02_http_poller_pool` | HTTP + `ztk_poller_pool`（size=2） |
| `demo_03_udp_unicast_recv` | UDP server + poller |
| `demo_04_udp_unicast_send` | UDP client + poller |
| `demo_05_tcp_udp_shared_poller_pool` | TCP + UDP sharing explicit `ztk_poller_pool` (ZMS live path) |

## Build one demo

```bash
cmake -S .. -B ../build -DCMAKE_BUILD_TYPE=Release
cmake --build ../build --target demo_01_rtsp_manual_poll
```

Low ports (80, 554) may require root on Linux. Override at compile time:

```bash
cmake -S .. -B ../build -DCMAKE_C_FLAGS="-DDEMO_RTSP_PORT=8554 -DDEMO_HTTP_PORT=8080"
```

## More reference code

- Unit tests: [`../tests/`](../tests/)
- Documentation: [`../docs/README.md`](../docs/README.md)（01～10 编号指南）
