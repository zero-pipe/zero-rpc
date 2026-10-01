# zrpc Architecture

## Goals

`zrpc_node` is the process-level runtime. A node owns the poller pool, service
registry, route table, pending calls, codec registry, discovery client, and one
or more transport bindings. `zrpc_service` exposes server methods. `zrpc_proxy`
starts client calls.

The application layer must not depend on sockets, TCP sessions, UDP RTP state,
discovery packet layouts, or a concrete serializer.

## Layers

```text
application API
  node / service / proxy / payload
          |
RPC core
  request dispatch / pending calls / reply token
          |
contract and routing
  service.method / endpoint set / selector
          |
registry and discovery
  static routes / mesh registry / future KV or watch backend
          |
transport facade
  scheme + endpoint + message adapter
          |
transport provider
  tcp / udp / dds / future providers
          |
zero-tool-kit
  poller / socket / TCP / UDP / timer / buffer
```

Dependencies point downward. A transport provider delivers a complete
`zrpc_transport_message_t` to the node callback; it does not call a service
handler directly. A service handler sees only a request view and a reply call.

## Node And Binding

The old `node.transport` and `node.data_port` fields remain as a compatibility
configuration path. New configurations use:

```json
{
  "transport": {
    "bindings": [
      { "name": "tcp", "endpoint": "tcp://0.0.0.0:8080" },
      { "name": "udp", "endpoint": "udp://0.0.0.0:8081" }
    ]
  }
}
```

Each configured binding is started independently. A request selects a route
endpoint by scheme; a reply uses the transport binding that received the
request. This prevents the first node transport from becoming an accidental
global transport.

## Routing And Mesh

The route value is now an endpoint set, not a single `service -> ip:port`
value. Static routes take precedence over mesh routes. Dynamic mesh routes are
rebuilt from peer snapshots and can contain TCP, UDP, or registered custom
schemes. Selection currently uses round-robin endpoint selection; load
balancing and health policy should be added above the route table.

The built-in mesh control frame advertises all node bindings and service names.
It is intentionally kept behind the discovery module. A future KV/watch mesh
backend should produce the same peer/service/endpoint snapshot and must not
leak its key-value protocol into the transport drivers.

The intended registry key shape is:

```text
zrpc/registry/<node_name>/<service_name>/<instance_id>
```

The value is an endpoint list. Registry backends should update snapshots by
service and instance, then notify the router. They should not modify service
handlers or transport framing.

The current implementation now has these explicit boundaries:

```text
zrpc_registry_t
    service + instance_id + node_id + endpoint[] + lease timestamp

zrpc_registry_backend_t
    start / stop / tick / announce

selector
    static route first, registry snapshot second
```

The currently supported control-plane sources are deliberately limited to:

```text
static routes
mesh backend
```

The KV/watch backend is only a reserved extension point. It is not enabled,
configured, or required by the current runtime. If it is added later, it only
needs to implement `zrpc_registry_backend_ops_t` and write the same registry
snapshot; proxy and transport code do not change.

## Large Payload Policy

The node has a configurable `max_msg_bytes` limit. The default remains 1 MiB;
the current hard safety ceiling is 64 MiB. The limit applies before allocation
and during TCP/UDP receive reassembly, so a peer cannot force an unbounded
buffer allocation.

Proxy options also expose an optional `prefer_stream_threshold`:

```c
zrpc_proxy_options_t options = {0};
options.prefer_stream_threshold = 16 * 1024;
```

When enabled and a request payload exceeds the threshold, selector lookup
prefers a TCP endpoint for the service. If no TCP endpoint is available, it
falls back to the normal endpoint policy. This is intentionally a selector
policy, not a transport hard switch: a node with only UDP remains usable.

The 16 KiB value is a policy default for deployment, not a wire-protocol
constant. It should be tuned using benchmark data and network characteristics.

## Performance Decisions

`ztk_socket_sendv` is available, so TCP has a scatter/gather fast path:

```text
length/envelope header + route header + borrowed payload
```

The payload avoids an intermediate frame copy when the socket can accept the
write. If backpressure causes a partial write, the unsent suffix is copied into
the existing per-link queue so the borrowed application buffer is never held
after the API call returns.

UDP still copies datagram chunks into the RTX ring. This is deliberate: NACK
and RTX require retained datagrams, and `sendto_batch` requires contiguous
datagrams. A future UDP `sendmsg` path can trade batching for scatter/gather,
but should be benchmarked before replacing the current path.

## Deferred Ideas From The Proposal

- Streaming chunk callbacks are useful for large file/image/message flows, but
  need a streaming RPC contract because method dispatch and response ownership
  currently operate on a complete envelope.
- A full-message callback remains the default for ordinary RPC and codec-based
  protobuf/JSON handling.
- FlexFEC and jitter buffering are not enabled. RPC already has NACK/RTX, and
  media-style playout buffering would add latency and memory without a current
  RPC requirement.
- RFC2326 `$` interleaved framing is not used. zrpc already multiplexes logical
  requests with request IDs over a length-prefixed TCP stream; adding a second
  media framing layer would be redundant.

## Message And Codec Ownership

Transport providers carry an RPC message envelope plus opaque payload bytes.
The payload `encoding` tag belongs to the application codec registry. TCP
framing, UDP RTP/ARQ framing, and future DDS framing are provider concerns;
business serialization is not.

The code now reflects that split:

```text
message/zrpc_envelope.c
    provider-neutral envelope: kind / encoding / request_id / route / payload

transport/zrpc_wire.c
    UDP RTP + fragment framing, and TCP length-prefix framing (opaque bytes)

transport/zrpc_udp.c
    socket/session plumbing, batching, congestion budget, and provider callbacks

transport/zrpc_rx_window.c
    sequence window and missing-sequence detection

transport/zrpc_reasm.c
    bounded fragment reassembly slots and expiry

transport/zrpc_tx_ring.c
    retained datagrams for NACK-triggered RTX

transport/zrpc_tcp.c
    stream framing over opaque envelope bytes
```

The UDP layer never inspects RPC fields anymore: it fragments and reassembles the
encoded envelope as opaque bytes, then hands the completed envelope to
`zrpc_envelope_decode`. The TCP layer does the same with a single length-prefix
frame. Only the envelope codec understands `kind / encoding / request_id /
route`; only the application codec understands the payload body.

## Provider Extension

Applications register a provider before creating a node:

```c
static const zrpc_transport_provider_t dds_provider = {
    "dds", &dds_provider_ops
};

zrpc_transport_provider_register(&dds_provider);
```

The provider owns its instance and opaque reply tokens. `send` returning
`ZRPC_OK` means the provider accepted the message for delivery, not that the
remote handler completed. Provider callbacks must be non-blocking and obey the
node poller threading contract.

## Migration Stages

1. Completed: endpoint model, multiple node bindings, endpoint-set routes,
   multi-binding mesh advertisement, and provider lifecycle SPI.
2. Completed: registry backend SPI, selector, and static-route-first lookup.
3. Completed: provider-neutral envelope separated from TCP/UDP framing.
4. Completed: extracted UDP receive-window, reassembly, and RTX ring modules.
5. Later: add metadata, trace, deadline, health, load-balancing, and generated service
   contracts without changing the transport providers.
