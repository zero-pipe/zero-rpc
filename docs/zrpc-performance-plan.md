# zrpc Performance And Large Payload Plan

## Scope

This document defines the performance rules for the C99 implementation. It is
the source of truth for future transport, streaming, and benchmark work.

## Current Rules

### Message Size Limit

`zrpc_node_config_t.max_msg_bytes` controls the maximum logical envelope size.

```text
default: 1 MiB
hard safety ceiling: 64 MiB
```

The limit is checked before sending, before TCP receive-buffer growth, and
before UDP reassembly allocation. A peer cannot cause an unbounded allocation
by advertising a large fragment total.

### UDP

UDP is the preferred transport for small, latency-sensitive RPC when the route
contains a UDP endpoint.

```text
UDP datagram payload: configured frag_bytes, default 1200 bytes
IP fragmentation: forbidden by application design
loss recovery: receive window + NACK + RTX
large message handling: envelope fragmentation and bounded reassembly
```

The UDP reliability modules do not understand RPC fields:

```text
zrpc_rx_window.c  -> sequence acceptance and missing sequence detection
zrpc_reasm.c      -> bounded fragment buffers and expiry
zrpc_tx_ring.c    -> retained datagrams for RTX
```

UDP datagrams are retained in the RTX ring. Therefore the current UDP path is
not fully zero-copy. This is intentional because NACK recovery requires a
stable datagram copy.

### TCP

TCP carries one opaque envelope per length-prefixed frame. TCP has a scatter /
gather fast path using `ztk_socket_sendv`:

```text
length + envelope header + route + borrowed payload
```

If the socket accepts the complete write, the large payload avoids an
intermediate frame copy. If backpressure causes a partial write, only the
unsent suffix is copied into the per-link queue. The borrowed application
buffer is never retained after the call returns.

TCP does not use RFC2326 `$` interleaved framing. zrpc already multiplexes
requests with request IDs over the TCP stream, so a second media framing layer
would be redundant.

## Optional Size-Based Endpoint Preference

Size-based selection is a selector policy, not a transport rule.

```c
zrpc_proxy_options_t options = {0};
options.prefer_stream_threshold = 16 * 1024;
zrpc_proxy_create(node, &options, &proxy);
```

Rules:

1. `prefer_stream_threshold == 0` disables the policy.
2. If payload length is greater than the threshold, selector prefers TCP.
3. If the service has no TCP endpoint, selector falls back to UDP or another
   available endpoint.
4. Direct proxy endpoint configuration always wins over selector policy.
5. The threshold is not part of the wire protocol and must be benchmarked per
   deployment.

This preserves nodes that only bind UDP and avoids coupling the transport
provider to business payload size.

## User Delivery Modes

### Full Message Mode

Current default. The service handler receives a complete payload:

```c
typedef void (*zrpc_handler_fn)(zrpc_call_t *call,
                                const zrpc_request_t *request,
                                void *user);
```

Use this mode for:

- payloads that fit the configured message limit
- protobuf/JSON decoding
- image or object decoding that requires a complete buffer
- ordinary request/response RPC

### Streaming Chunk Mode

Phase 1 is now implemented for both upload and download directions. It is the
intended API for large files, model blobs, and stream-like RPC.

The proposed C API is:

```c
typedef struct zrpc_chunk_context {
    uint32_t request_id;
    uint32_t stream_id;
    uint64_t offset;
    uint64_t total_size;       /* 0 when unknown */
    int first;
    int last;
    const uint8_t *data;       /* borrowed; valid only during callback */
    size_t len;
} zrpc_chunk_context_t;

typedef void (*zrpc_chunk_handler_fn)(zrpc_call_t *call,
                                      const zrpc_chunk_context_t *chunk,
                                      void *user);
```

The server can send response chunks with `zrpc_reply_stream`; the client can
receive them through `zrpc_proxy_call_stream`. The pending request remains
open until a response chunk carries `ZRPC_STREAM_LAST`.

Streaming rules:

1. The first envelope identifies service, method, request ID, encoding, and
   stream metadata.
2. Following chunks are opaque payload bytes and do not repeat service/method
   strings.
3. The callback must consume or copy data before returning.
4. The transport may reuse the receive buffer after the callback returns.
5. `last == 1` terminates the stream.
6. A stream timeout and maximum total size are mandatory.
7. A stream handler and a full-message handler are mutually exclusive for one
   method registration.
8. A stream handler must be able to return an application error, cancellation,
   or backpressure decision.

For a streaming receiver, memory target is `O(number_of_inflight_chunks)` and
must not require one contiguous allocation for the full message.

## Deferred Features

### FEC

FlexFEC is deferred. NACK/RTX is already implemented and is easier to verify
for RPC request/response semantics. FEC should only be added after packet-loss
benchmarks show that retransmission latency is the dominant problem.

### Jitter Buffer

A media-style jitter buffer is deferred. RPC usually needs completion and
ordering, not playout scheduling. A future streaming API may add an explicit
ordered delivery policy without making it a transport requirement.

### Full UDP Zero-Copy

`sendto_batch` requires contiguous datagrams and RTX requires retained bytes.
Replacing it with `sendmsg` would trade batching for scatter/gather. This must
be benchmarked as a separate provider option rather than enabled by default.

## Benchmark Matrix

Every performance change must run this matrix:

| Case | Transport | Payload | Expected purpose |
|---|---|---:|---|
| A | UDP | 64 B | latency and closed-loop QPS |
| B | UDP | 8 KiB | fragmentation and reassembly |
| C | TCP | 64 B | stream framing baseline |
| D | TCP | 32 KiB | TCP sendv and large payload path |
| E | UDP | 32 KiB | multi-fragment UDP reliability |
| F | 4 concurrent UDP clients | 64 B | contention and stability |

Collect:

- attempted/succeeded/failed requests
- throughput
- min/mean/max latency
- timeout, not-found, I/O, and protocol error counts
- RSS and allocation behavior for large messages

No performance result is accepted if functional tests or the reliability unit
tests fail.
