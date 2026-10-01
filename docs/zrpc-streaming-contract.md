# zrpc Streaming Contract

This document defines the next streaming API without changing the current
full-message RPC handler semantics.

## Two Delivery Modes

Every method has one delivery mode:

```text
FULL_MESSAGE
    Current default. The framework reassembles the complete envelope payload
    before invoking zrpc_handler_fn.

STREAMING_CHUNK
    Future API. The framework invokes a chunk callback as fragments arrive and
    does not allocate one buffer for the complete payload.
```

A method cannot use both modes for one call. The mode is part of the service
method contract and must be known before the first payload chunk is delivered.

## Implemented Phase 1 API

Phase 1 supports client -> server chunked request streaming and server -> client
chunked response streaming over both UDP and TCP:

```c
typedef struct zrpc_chunk {
    const void *data;
    size_t len;
    uint8_t encoding;
    uint64_t offset;
    unsigned flags; /* ZRPC_STREAM_FIRST / ZRPC_STREAM_LAST */
} zrpc_chunk_t;

typedef void (*zrpc_chunk_handler_fn)(zrpc_call_t *call,
                                      const zrpc_request_t *req,
                                      const zrpc_chunk_t *chunk,
                                      void *user);

int zrpc_service_add_stream_method(zrpc_service_t *svc, const char *method,
                                   zrpc_chunk_handler_fn fn, void *user);

int zrpc_proxy_stream_open(zrpc_proxy_t *proxy, const char *service, const char *method,
                           zrpc_response_fn cb, void *user, uint64_t timeout_ms,
                           zrpc_stream_t **out_stream);
int zrpc_proxy_stream_send(zrpc_stream_t *stream, const zrpc_chunk_t *chunk);
int zrpc_proxy_stream_cancel(zrpc_stream_t *stream);
int zrpc_proxy_stream_destroy(zrpc_stream_t *stream);

typedef void (*zrpc_response_chunk_fn)(int status,
                                      const zrpc_chunk_t *chunk,
                                      void *user);
typedef void (*zrpc_stream_done_fn)(int status, void *user);

int zrpc_proxy_call_stream(zrpc_proxy_t *proxy, const char *service, const char *method,
                           const zrpc_payload_t *request,
                           zrpc_response_chunk_fn on_chunk,
                           zrpc_stream_done_fn on_done,
                           void *user, uint64_t timeout_ms,
                           uint64_t *out_request_id);

int zrpc_reply_stream(zrpc_call_t *call, int status, const zrpc_chunk_t *chunk);
```

Phase 1 rules:

1. The client sends chunks with monotonic `offset`; the first carries
   `ZRPC_STREAM_FIRST`, the last carries `ZRPC_STREAM_LAST`.
2. Each chunk is carried by the provider-neutral envelope with stream metadata
   (`stream_flags`, `stream_id`, `stream_offset`, `stream_total_size`).
3. UDP fragments the envelope through the existing reliability engine; TCP uses
   the length-prefixed envelope. Streaming does not change provider framing.
4. The server upload handler may reply from the `LAST` callback.
5. A normal handler may emit multiple response chunks with
   `zrpc_reply_stream`; the client remains pending until response `LAST`.
6. A non-stream response remains supported and is delivered through the normal
   unary callback.
7. All responses are correlated by `request_id`.
8. A stream send is rejected with `ZRPC_ERR_STATE` when sequence rules are
   violated (offset not contiguous, FIRST not first, send after LAST).
9. `zrpc_proxy_stream_destroy` succeeds only after the response closed the
   stream.

Phase 1 behavior notes:

- Only the `FIRST` chunk carries the `service.method` route on the wire; the
  server caches the resolved method in its per-stream state and dispatches
  subsequent chunks without re-parsing the route.
- `zrpc_proxy_stream_cancel` sends a best-effort `CANCEL` envelope to the peer
  and completes the local request with `ZRPC_ERR_CLOSED`.

Phase 1 limitations (explicitly deferred):

- One writer thread per stream.
- No backpressure propagation to the sender.
- `CANCEL` is best-effort: a response already in flight may still arrive.

## Target C API (later phases)

```c
typedef struct zrpc_chunk_context {
    uint32_t request_id;
    uint32_t stream_id;
    uint64_t offset;
    uint64_t total_size;       /* 0 if unknown */
    int first;
    int last;
    const uint8_t *data;       /* borrowed; valid during callback only */
    size_t len;
} zrpc_chunk_context_t;

typedef int (*zrpc_chunk_handler_fn)(zrpc_call_t *call,
                                     const zrpc_chunk_context_t *chunk,
                                     void *user);
```

The method registration API should make the choice explicit rather than
inferring it from payload size:

```c
zrpc_service_add_stream_method(service, "upload",
                                on_upload_chunk, user);
```

The exact public function name is not frozen until the wire state machine and
backpressure behavior are implemented.

## State Machine

```text
IDLE
  -> OPEN on first chunk
OPEN
  -> OPEN for intermediate chunks
OPEN
  -> COMPLETED on last chunk
OPEN
  -> FAILED on protocol error, timeout, size limit, or handler error
OPEN
  -> CANCELLED on local/remote cancellation
```

Required invariants:

1. `first == 1` occurs exactly once.
2. `last == 1` occurs exactly once for a successful stream.
3. Offsets are monotonic and never overlap.
4. A chunk callback receives borrowed memory only for the callback duration.
5. The handler must copy data if it outlives the callback.
6. The transport may reuse the receive buffer immediately after callback return.
7. `max_stream_bytes` and `stream_timeout_ms` are mandatory limits.
8. Handler return values must support continue, complete, fail, and backpressure.
9. A cancelled stream must not invoke the complete callback afterward.
10. The response is sent only after the stream reaches COMPLETED or FAILED.

## Wire Requirements

The existing provider-neutral envelope remains the control envelope. A future
streaming envelope should add stream metadata rather than overload `route`:

```text
message kind
request id
stream id
stream flags: FIRST / LAST / CANCEL
chunk offset
total size (optional)
payload encoding
```

TCP carries stream chunks as length-prefixed envelopes. UDP carries stream
chunks as opaque fragments through the existing reliability engine. The UDP
engine must not understand service/method or business encoding.

## Full Message Safety

Until this API is implemented, all methods remain `FULL_MESSAGE`. Large
payloads are protected by:

- `max_msg_bytes`
- bounded reassembly slots
- reassembly timeout
- RTX ring bounds
- TCP receive-buffer limits

No code should expose a fake chunk callback by calling the full-message handler
once per transport fragment. That would violate envelope semantics and break
codecs that require the complete payload.
