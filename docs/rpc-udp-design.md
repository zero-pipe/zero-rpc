# zero-rpc 设计文档：基于 UDP + RTP/RTCP 的低延时可靠 RPC

> 状态：设计草案（v0.1）
> 依赖：仅 zero-tool-kit（ZTK）
> 参考素材（只学思路、不复用代码/不引入依赖）：`app/`（接口风格）、`kvs-webrtc-sdk`（JitterBuffer/NACK/SRTP 算法）、`mesh/`（发现面思路）

---

## 1. 目标与非目标

### 1.1 目标

- **低延时**：单条约 1 个 RTT 内完成；无连头阻塞（HOL）。
- **抗弱网**：丢包/抖动/乱序下仍可用（选择性重传 + 拥塞控制 + pacing）。
- **高性能**：批处理、零拷贝、无锁、每核并行，能压满多核。
- **可发现**：服务发现走 UDP 广播，节点上下线可感知。
- **安全**：数据面加密（SRTP），发现面可校验。

### 1.2 非目标

- 不做媒体播放语义（**不引入 playout jitter buffer**）。
- 不追求与第三方 WebRTC/媒体设备互通（不接入 KVS 的 ICE/DTLS/SCTP 栈）。
- 不支持大于 `RPC_MSG_MAX` 的单条消息（建议默认 4 MiB，可配）。

---

## 2. 总体架构

```
┌──────────────────────────── 应用层 ────────────────────────────┐
│  rpc_node  →  rpc_service (register)  /  rpc_proxy (call)       │
│                         │                                       │
│                    rpc_stub（请求-响应关联 / 超时 / 重试）        │
│                         │ send(msg)  ← 可插拔 transport          │
├─────────────────────────┼───────────────────────────────────────┤
│                 udp_rtp_transport（数据面）                      │
│  ┌──────────────┬──────────────┬───────────────┬─────────────┐ │
│  │ 消息分片/重组 │  ARQ(NACK/RTX)│ 拥塞控制+pacing│  SRTP 加解密 │ │
│  └──────────────┴──────────────┴───────────────┴─────────────┘ │
├─────────────────────────┼───────────────────────────────────────┤
│           ZTK：ztk_poller_pool / ztk_socket(UDP) / timer / buf   │
└─────────────────────────────────────────────────────────────────┘
        ▲ 控制面（独立轻量通道，不走 RTP）
        └── ztk_socket UDP 广播：HELLO / BYE / 服务表增量
```

**分层原则**

- **控制面 = 发现**：低频、可周期重发、明文可签名；只回答“谁能提供哪个服务、地址端口多少”。
- **数据面 = RPC**：高频、低延时、端到端可靠（选择性重传），走 RTP/UDP。
- 控制面与数据面**互不依赖**：发现挂了不影响已建立的数据通道；数据面不参与发现。

---

## 3. 依赖与复用

### 3.1 ZTK 已提供（直接使用）

| 能力 | ZTK API |
| --- | --- |
| 多核事件循环 | `ztk_poller_pool_create/start`、`ztk_poller_pool_get_by_key`（连接钉核） |
| UDP socket | `ztk_socket_bind_udp_ex`（含 `SO_REUSEPORT`）、`ztk_socket_sendto`、`ztk_socket_recvfrom` |
| 广播/组播 | `ztk_socket_set_broadcast`、`ztk_socket_join_multicast`、`ztk_socket_set_multicast_ttl/if/loop` |
| 定时器（pacing/RTO） | `ztk_poller_do_delay`、`ztk_timer_start` |
| 内存池 | `ztk_buf`（引用计数）、`ztk_buf_pool`（512B~2M 分档）、`ztk_poller_pool_attach_buf_pools` |
| 无锁队列 | `ztk_mpsc`（多生产者单消费者） |
| 墙钟 | `ztk_wall_ms` |

### 3.2 ZTK **没有**、必须自建或补的

见第 12 节。核心三项：**UDP 批量收发**、**UDP 拥塞控制+pacing**、**SRTP**。

> 注意：`ztk_socket_set_bbr` 实际是 `ztk_sockplat_set_tcp_bbr`，**只作用于 TCP**，对 UDP 无效。UDP 的 CC 必须我们自己在传输层做。

### 3.3 参考素材的使用边界

- **`app/`**：只借接口形状（node/service/proxy/stub + 可插拔 `send` 回调 + 引用计数 buf + buf_chain 的 iovec）。其 `fc_*`/`framework/*`/`osi/*` 头文件不在本仓库，不引入。
- **`kvs-webrtc-sdk`**：只借算法思路（`JitterBuffer.c` 的重排窗口、`Retransmitter.c` 的 NACK/RTX、`SrtpSession.c` 的 SRTP 处理）。**不引入其 ICE/DTLS/SCTP/PeerConnection 栈**。
- **`mesh/`**：只借“广播发现 + 服务表”的思路；重写为 ZTK UDP 上的精简二进制协议。

### 3.4 建议模块划分（目录 `rpc_udp/`）

| 模块 | 职责 |
| --- | --- |
| `rpc_transport` | 抽象接口：`send(msg)`、`on_deliver`、`on_fail`、`start/stop`；`rpc_udp` 是其中一种实现。 |
| `rpc_udp_channel` | 一个 peer 对的会话：SSRC、seq 空间、cwnd、RTT、重传缓存、重组窗口。 |
| `rpc_udp_arq` | 发送端重传 ring + 接收端 gap 检测/NACK 调度 + RTX 发送。 |
| `rpc_udp_cc` | 拥塞控制（初版 AIMD）+ pacing。 |
| `rpc_udp_rtp` | RTP/RTCP 报文编解码（NACK/RR/RTX 组装解析）。 |
| `rpc_udp_srtp` | SRTP/SRTCP 加解密（后续阶段）。 |
| `rpc_discovery` | 控制面：HELLO/BYE 广播 + registry + TTL。 |

---

## 4. 数据面协议

### 4.1 为什么用 RTP 框架

选择 RTP 的唯一硬理由：**SRTP 天然以 RTP 帧为单位**，直接复用可省掉自研加密的封装与防重放设计；同时 RTCP 现成的 NACK/RR 反馈语义可复用。代价是头部比自研大（12B vs 4~8B），可接受。

### 4.2 RTP 头用法（12 字节，无 CSRC / 无扩展的常规情形）

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|X|  CC   |M|     PT      |       sequence number         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           timestamp                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           synchronization source (SSRC) identifier            |
+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
|                 RPC 子头（见 4.3）                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      用户 payload                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| 字段 | 取值与含义 |
| --- | --- |
| V | 固定 2 |
| P | 是否 padding；短消息恒 0，大消息可选对齐 |
| X | 0（本版不用 RTP 扩展头，RPC 子头放在 payload 起始） |
| CC | 0 |
| M | **末分片标志**：该消息最后一个分片置 1；单包消息恒 1 |
| PT | 动态类型：`96 = RPC 数据`，`97 = RTX 重传`；控制面不用 RTP |
| sequence | 每 SSRC 单调递增 16 位序号，回绕按 RFC 1982 处理 |
| timestamp | 本版复用为**发送时刻（毫秒低 32 位）**，仅本端点解释，用于单向时延/RTO 估计。（非标准用法，文档化即可） |
| SSRC | 标识一条**单向可靠流**（一个 peer 一个方向一个 SSRC）；每条该流一个 RTX SSRC（RFC 4588） |

### 4.3 RPC 子头（紧跟 RTP 头，位于 payload 起始）

小端字段用网络序；固定 16 字节，4 字节对齐，便于解析。

```c
typedef struct rpc_hdr {
    uint32_t msg_id;      /* 请求/响应关联 ID（发送方分配，响应原样返回） */
    uint32_t msg_len;     /* 整条消息总长（字节），仅首分片必需，其余可填同值 */
    uint16_t frag_off;    /* 本分片在消息中的字节偏移 */
    uint16_t frag_len;    /* 本分片携带的用户字节数 */
    uint8_t  kind;        /* 0=request 1=response 2=event */
    uint8_t  flags;       /* bit0=FIRST, bit1=LAST, bit2=URGENT, ... */
    uint16_t rsv;
} rpc_hdr_t;             /* 16 bytes */
```

- MTU 与分片：数据分片大小 = `min(路径 MTU, 配置) - IP/UDP/RTP/RPC 头`。初版可固定 `RPC_FRAG_PAYLOAD = 1200`，后续接 PMTU 探测。
- **小于等于单分片的消息（绝大多数 RPC）只有 1 个包，走“直通路径”：收到即投递，不进任何重排队列**，这是 RPC 低延时的关键。

### 4.4 消息模型与多路复用

- 一条 UDP 四元组上承载**多条逻辑流**（不同 SSRC），每条流内承载**多条并发消息**（不同 `msg_id`）。
- `msg_id` 由发送端单调分配，`(SSRC, msg_id)` 唯一标识一条消息；响应消息沿用请求的 `msg_id`。
- 请求/响应通过 `msg_id` + `kind` 关联，不依赖 TCP 连接状态。

---

## 5. 可靠性设计（选择性重传 / ARQ）

> 明确：**不做媒体 playout jitter buffer**。RPC 要的是“缺口才等、齐了就投递”；jitter buffer 的固定播放延时对 RPC 是负优化。

### 5.1 发送端

- 每条流维护一个**重传环形缓冲**（按 seq 索引，容量可配，默认 1024 包/流），保存已发未确认的 RTP 包用于 NACK 重发。
- 收到 NACK 后：命中则**在 RTX 流上重发**（RFC 4588：独立 SSRC + PT=97，payload 前 2 字节放原始 seq/OSN）；未命中（太旧被驱逐）则忽略（接收端会超时失败）。
- 重传次数上限 + 总时长上限，超限标记消息失败并通知上层（可回退到发现面找到的 TCP 通道，见 §13 待定项）。

### 5.2 接收端

- 每条流维护**重组窗口**：预期 seq + 一个**有界**乱序缓冲（默认窗口 256 包）。
- 窗口内到齐即重组投递（大消息按 `frag_off` 拼接，收到 `LAST` 且无空洞才算完成）。
- 单包消息立即投递，不等待。

### 5.3 NACK 时机

- **缺口触发**：发现空洞且后续包已到达 → 立即发一次 NACK。
- **定时触发**：距上次 NACK 超过 `NACK_RTO` 仍未补齐 → 再发（带指数退避，最多 N 次）。
- NACK 用 RTCP `RTPFB`（PT=205, FMT=1）：`PID`(16b) + `BLP`(16b) 位图，可一次请求多个连续缺口。

### 5.4 RTT / RTO

- 通过 RTCP `RR` 的 `LSR/DLSR` 计算 RTT；或用 RTP timestamp 的发送时刻做单向估计。
- `RTO = SRTT + 4*RTTVAR`（Jacobson/Karels），下限可配；用于 NACK 重发定时与消息超时。

---

## 6. 拥塞控制与 pacing

“抗弱网”的前提，**不可省**。

- **初版**：AIMD 窗口 + pacing。以 RTCP RR 的丢包率与 RTT 驱动：
  - 丢包低于阈值：慢启动/加性增窗；达到阈值：乘性减窗（类 Reno）。
  - pacing 用 `ztk_poller_do_delay` 按 `rate` 均匀出包，避免突发丢包。
- **进阶**：BBR 风格（基于带宽/RTprop 估计），参考 ZTK 内 TCP BBR 思路但作用于 UDP 发送速率。
- **背压**：应用提交速率超过 CC 允许速率时，在 `rpc_stub` 层排队并返回 `ERR_AGAIN` 或异步回调，避免无界积压。
- 需暴露可观测指标：`rate`、`inflight`、`loss`、`rtt`、`retrans`，便于压测定位。

> 说明：ECN/L4S 可作为后续增强（需 ZTK 暴露 `IP_TOS`/`SO_...ECN`，见 §12）。

---

## 7. 安全（SRTP）

- 数据面用 **SRTP** 加密 + 防重放（参考 `kvs-webrtc-sdk/Srtp/SrtpSession.c` 算法，用 ZTK 已有的 OpenSSL 依赖或自带 AES-GCM）。
- 密钥协商：初版可 `PSK / 控制面下发`；后续演进到简化握手（派生会话密钥），不引入完整 DTLS。
- 控制面 HELLO/BYE：加 **HMAC/签名** 防投毒；服务表条目带 TTL，异常条目不覆盖合法来源（沿用 mesh 的 `source_mask` 思路）。

---

## 8. 发现面协议（UDP 广播）

用 ZTK UDP + 广播/组播，**精简二进制**（不用 JSON，减少解析开销与包体）。

```
HELLO 帧：
  magic(4) | version(1) | flags(1) | rsv(2) | node_id(4) | addr(4) | seq(4)
  | entry_count(2) | entries[...]
entry:
  service_id(4)  // 服务名 hash 或全局注册 ID
  port(2) | proto(1) | weight(1) | ttl_sec(2)  // 数据面 UDP 端口

flags: bit0=HELLO, bit1=BYE, bit2=RELAYED
```

- 周期广播（带抖动）+ 变更立即广播；`BYE` 通知下线。
- 服务表：`(service_id, node_id)` 为主键，带 generation/TTL，冲突按 generation 新者胜。
- 同机多进程用 `SO_REUSEPORT`（ZTK `ztk_socket_bind_udp_ex` 已支持）。
- 跨子网：初版不做；如需，复用发现面的中继通道（TCP 或单播 UDP）——列为待定项。

---

## 9. 接口设计（对齐 app/ 风格）

```
rpc_node       生命周期 + 配置 + 挂载发现
rpc_service    注册 service_name / method / handler
rpc_proxy      call / call_async / call_sync
rpc_stub       请求-响应关联、超时、重试；持有可插拔 transport
transport      函数表：send / on_packet / on_feedback / close
```

关键：**传输层可插拔**（对齐 `app` 里 `fc_node_proxy_options` 把 `send` 回调传进 stub 的做法）。UDP+RTP 只是一种 transport 实现，接口层不感知 UDP/RTP。

```c
typedef struct rpc_transport {
    int  (*start)(struct rpc_transport*, const rpc_transport_cfg_t*);
    int  (*send)(struct rpc_transport*, const char* peer, uint16_t port,
                 uint32_t msg_id, uint32_t kind, const void* data, size_t len);
    void (*on_deliver)(...);   /* 完整消息回调 */
    void (*on_fail)(...);      /* 消息失败/超时 */
    void (*stop)(struct rpc_transport*);
    void* self;
} rpc_transport_t;
```

---

## 10. 线程与 IO 模型

- `ztk_poller_pool` 起 N = 核数个 IO 线程；每线程 `SO_REUSEPORT` 绑同一 UDP 端口，内核分担入站包。
- **连接/流钉核**：`ztk_poller_pool_get_by_key(peer_hash)`，保证同一 peer 的会话状态无跨线程锁。
- 每 poller 挂**本地 buf 池**（`ztk_poller_pool_attach_buf_pools`，`thread_safe=0`）避免锁。
- 跨线程投递用 `ztk_mpsc` + `ztk_poller_async`。
- 所有热路径（收包→重组→投递）尽量在单线程内闭合，避免跨核同步。

---

## 11. 性能优化清单（“炸裂”的真正来源）

1. **UDP 批量收发**：`recvmmsg/sendmmsg`（Linux）、`WSARecvMsg`/GQCS（Win）——一次系统调用处理多包。（ZTK 待补，见 §12）
2. **pacing + 批量出包**：定时器攒一批再 `sendmmsg`。
3. **零拷贝**：`ztk_buf` 引用计数贯穿收/重组/投递；发送用 `ztk_socket_sendv` 聚合头+payload（避免 memcpy）。
4. **无锁**：连接钉核 + 本地 buf 池 + `ztk_mpsc`。
5. **快路径直通**：单包消息不进重排队列、不加延时。
6. **避免 JSON / 频繁 malloc**：发现面二进制；数据面全程内存池。
7. **CPU 亲和/优先级**：`ztk_poller_pool_opts.thread_priority` + 线程绑核。

---

## 12. ZTK 缺口清单（需要新增/扩展的 API）

按优先级排列。

### P0 — 性能前提

**12.1 UDP 批量发送**
```c
typedef struct ztk_udp_msg {
    const void *data; size_t len;
    const char *ip; uint16_t port;
} ztk_udp_msg_t;
/* Linux: sendmmsg；Win: WSASendMsg 循环或 GQCS；返回已发送条数 */
ZTK_API int ztk_socket_sendto_batch(ztk_socket *sock, const ztk_udp_msg_t *msgs, unsigned count);
```

**12.2 UDP 批量接收**
```c
typedef struct ztk_udp_recv_msg {
    void *data; size_t cap; size_t len;
    char ip[46]; uint16_t port;
} ztk_udp_recv_msg_t;
/* Linux: recvmmsg；返回已收条数（<=cap） */
ZTK_API int ztk_socket_recvfrom_batch(ztk_socket *sock, ztk_udp_recv_msg_t *msgs, unsigned cap);
```

### P1 — 网络质量与可观测

**12.3** DSCP/TOS 设置：`ztk_socket_set_tos(sock, uint8_t dscp)`。
**12.4** ECN 开关（L4S 预留）：`ztk_socket_set_ecn(sock, int on)`；及收包时回传 ECN 位。
**12.5** socket 级统计：丢包/错误/队列长度查询，供 CC 与压测使用。

### P2 — 安全与工具

**12.6** SRTP 辅助（可选放 ZTK 或本仓库 crypto 子模块）：AES-GCM + 防重放的 `ztk_srtp_session`。
**12.7** 仅 Windows：批处理/`WSARecvMsg` 的辅助封装。

> 非 ZTK 缺口、但本仓库要实现：ARQ 重传环形缓冲、重组窗口、CC/pacing 控制器、RTCP 打包解析、发现面协议、RPC 接口层。

---

## 13. 里程碑

- **M0 设计评审**：本文档定稿，锁定 RTP 用法 / 缺口清单。
- **M1 ZTK 补丁**：`sendto_batch` / `recvfrom_batch`（P0）+ 单测与基准。
- **M2 最小闭环**：广播发现 + 单包 RPC 收发（含请求-响应关联、超时），跑通即可。
- **M3 可靠性**：分片/重组 + NACK/RTX + RTO；压测乱序/丢包。
- **M4 拥塞控制**：pacing + AIMD，弱网（丢包/抖动/限速）压测对比 TCP/QUIC。
- **M5 安全与硬化**：SRTP + 发现面签名 + 观测指标 + 边缘用例。
- **M6 基线报告**：P50/P99/P999 延迟与吞吐，对比 TCP 与 QUIC，验证“是否真的炸裂”。

---

## 14. 待定问题（Open Questions）

1. **CC 算法**：AIMD 起步是否够？何时切 BBR？是否需要 ECL/L4S。
2. **多路径/中继**：跨子网时数据面是否走“发现面中继”（TCP 降级）还是 UDP 隧道。
3. **传输降级**：UDP 完全不通（NAT/防火墙）时是否自动回退 TCP 通道。
4. **消息大小上限**与背压策略（拒绝 vs 排队 vs 阻塞）。
5. **SRTP 密钥协商**的具体方式（PSK / 控制面下发 / 简化握手）。
6. **PMTU 探测**是否需要（初版固定 1200B 是否足够）。

---

## 附录 A：与既有参考的差异（避免误读）

| 参考 | 借用 | 不借用 |
| --- | --- | --- |
| `app/` | node/service/proxy/stub 接口形状、可插拔 send、buf/buf_chain | `fc_*`/`framework/*`/`osi/*` 具体实现与头文件 |
| `kvs-webrtc-sdk` | JitterBuffer 重排窗口思路、Retransmitter NACK/RTX、SRTP 处理 | ICE/DTLS/SCTP/PeerConnection/信令栈 |
| `mesh/` | 广播发现 + 服务表 + source_mask 防覆盖 | TCP relay 原样实现、JSON 编解码、`osi/*` 依赖 |
| `zero-media-kit` | RTP/RTCP 报文格式与 NACK 结构参考 | 媒体 codec payloader（H264/Opus 等）与播放语义 |
