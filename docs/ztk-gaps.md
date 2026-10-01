# ZTK 缺口清单（rpc-udp 需要的新增能力）

本清单基于 `3rdpart/zero-tool-kit`（v0.8.0-m8）现有代码核对得出。目标是明确"ZTK 已有什么、还缺什么"，避免重复造轮子。

## 0. 可直接复用（不要重复实现）

| 能力 | API |
| --- | --- |
| 多核事件循环 | `ztk_poller_pool_create/start`、`ztk_poller_pool_get_by_key` |
| UDP 绑定 + `SO_REUSEPORT` | `ztk_socket_bind_udp_ex` |
| 广播 / 组播 | `ztk_socket_set_broadcast`、`ztk_socket_join_multicast`、`ztk_socket_set_multicast_ttl/if/loop` |
| 毫秒定时器 | `ztk_poller_do_delay`、`ztk_timer_start` |
| 引用计数缓冲池 | `ztk_buf`、`ztk_buf_pool`、`ztk_poller_pool_attach_buf_pools` |
| 无锁 MPSC 队列 | `ztk_mpsc_create/push/pop` |
| 墙钟 / 单调毫秒 | `ztk_wall_ms`、`ztk_monotonic_ms` |
| TCP 批量写 | `ztk_socket_sendv` |

## 1. 缺口（按优先级）

| # | 缺口 | 优先级 | 影响 | 现状 | 建议新增 API |
| --- | --- | --- | --- | --- | --- |
| 1 | UDP 批量收发 | P0 | 吞吐 / CPU / 延迟 | 仅单包 `ztk_socket_sendto/recvfrom` | `ztk_socket_sendto_batch` / `ztk_socket_recvfrom_batch` |
| 2 | 亚毫秒/纳秒单调时钟 | P0 | pacing 精度 | `ztk_monotonic_ms` 仅毫秒 | `ztk_monotonic_ns`（或 `ztk_monotonic_us`） |
| 3 | 速率控制 / pacing 原语 | P1 | 抗弱网 / 公平性 | 无 | `ztk_rate_limiter`（token bucket） |
| 4 | UDP 拥塞控制 | P1 | 抗弱网 | 无（`set_bbr` 是 TCP 专用） | 我们在 `rpc_udp_cc` 实现；ZTK 提供 cwnd/统计钩子 |
| 5 | socket 统计（重传/丢包/队列） | P1 | 可观测性 / CC 反馈 | 无 | `ztk_socket_get_stats` |
| 6 | TOS / DSCP / ECN | P2 | 弱网 / L4S | 无 | `ztk_socket_set_tos`、ECN 位读取 |
| 7 | Windows 批量 IO（WSARecvMsg / RIO） | P2 | 平台对等 | 无 | 视 Windows 是否首批支持 |
| 8 | SRTP / AEAD + 重放保护 | P1 | 安全 | 无（TLS 仅 TCP client） | 建议放 `rpc_udp` 层，复用 OpenSSL |

## 2. 关键缺口详述

### 2.1 P0 — UDP 批量收发（#1）

现状：`ztk_udp_server` 收包是**逐包 `recvfrom` 循环**（见 `src/net/udp_server.c`），`ztk_socket_sendto` 也是单包。高 PPS 下 syscall 成为瓶颈。

需要的形态（示意）：

```c
/* 发送：一次 syscall 发多个数据报（Linux: sendmmsg） */
ztk_err_t ztk_socket_sendto_batch(int fd,
                                  const ztk_udp_dgram_t *msgs, size_t count,
                                  size_t *out_sent);

/* 接收：一次 syscall 收多个数据报（Linux: recvmmsg） */
ztk_err_t ztk_socket_recvfrom_batch(int fd,
                                    ztk_udp_dgram_t *msgs, size_t cap,
                                    size_t *out_count);
```

其中 `ztk_udp_dgram_t` 至少含 `data/len/ip/port`。平台差异：

- Linux：`sendmmsg`/`recvmmsg`。
- Win32：`WSASendMsg` 循环或 `WSARecvMsg`；高性能可用 RIO（Registered I/O）。

### 2.2 P0 — 高精度时钟（#2）

现状：`ztk_monotonic_ms()` 基于 `CLOCK_MONOTONIC` 但只返回毫秒。pacing 需亚毫秒分辨率，否则高速率下发送间隔量化误差大。

```c
uint64_t ztk_monotonic_ns(void);
```

### 2.3 P1 — 拥塞控制归属（#4）

ZTK 的 `ztk_socket_set_bbr` 实际是 `setsockopt(IPPROTO_TCP, TCP_CONGESTION, "bbr")`，**只对 TCP 生效**，UDP 无内核 CC。因此 UDP 侧 CC 必须在用户态实现。ZTK 需要提供的只是：高精度时钟（#2）、批量发送（#1）、以及可选的统计钩子（#5）。CC 算法本身放 `rpc_udp_cc`。

## 3. 交付建议

1. 先做 P0（#1、#2），它们是无条件收益、且不涉及复杂策略。
2. CC（#4）先在 `rpc_udp` 内做 AIMD，稳定后再评估是否下沉到 ZTK。
3. 安全（#8）评估用 OpenSSL 的 AEAD 实现 SRTP，避免手写密码学。
4. Windows 支持（#7）作为独立决策点，不阻塞 Linux 主线。
