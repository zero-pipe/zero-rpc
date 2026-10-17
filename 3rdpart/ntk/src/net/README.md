# net/

## M1：`ztk_socket`

| API | 说明 |
|-----|------|
| `ztk_socket_listen` | TCP 监听，非阻塞 |
| `ztk_socket_connect` | TCP 连接，支持 `EINPROGRESS` |
| `ztk_socket_bind_udp` | UDP 绑定 |
| `ztk_socket_accept` / send / recv / sendto / recvfrom | 基础 I/O |
| `ztk_socket_attach_poller` | 挂到 `ztk_poller`，事件回调 |

平台实现：

- Linux：`src/net/linux/sock_linux.c`（`getaddrinfo`、非阻塞、`MSG_NOSIGNAL`）
- Win32：`src/net/win32/sock_win32.c`（Winsock2 + `getaddrinfo`，非阻塞 TCP/UDP）

## M6：高层封装与组播

| 模块 | 头文件 |
|------|--------|
| TCP 客户端 | `ztk/net/tcp_client.h` |
| UDP 服务 | `ztk/net/udp_server.h`（多 poller + `SO_REUSEPORT`） |
| UDP 客户端 | `ztk/net/udp_client.h` |
| 组播/广播 | `ztk_socket_set_broadcast` / `join_multicast` 等 |

见 [docs/10-net-clients.md](../../docs/10-net-clients.md)。

## M2：`ztk_tcp_server`

- 依赖外部 `ztk_poller_pool`（I/O 由 pool 线程驱动；TcpServer 不创建 poller 线程）
- 同一 `listen fd` 注册到 pool 内每个 poller（抢占式 accept）
- `ztk_tcp_session_ops`：`on_recv` / `on_error` / 可选 `on_manager`
- 可选 `pick_poller` 做负载均衡；`start` 时自动挂载 manager 巡检 timer

## M3–M5

- `ztk_poller_async` / `ztk_poller_run` — 见 [docs/06-poller-async.md](../../docs/06-poller-async.md)
- `ztk_poller_do_delay` / `ztk_timer_start`（定时驱动 epoll 超时）
- `ztk_poller_pool` + 默认 `pick_poller` — 见 [docs/07-poller-pool.md](../../docs/07-poller-pool.md)
- TcpServer 用法 — [docs/09-tcp-server.md](../../docs/09-tcp-server.md)
- Win32 — [docs/04-win32.md](../../docs/04-win32.md)
