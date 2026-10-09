# Contract: RDMA Control and Data Planes

## Generated Control Path

`proto/rdma_control.proto` sets `option cc_generic_services = true` and is compiled with the normal Protobuf C++ generator only. The generated Stub invokes the following V1 path:

```text
Generated Stub
  -> RdmaConnection::CallMethod
  -> allocate request_id / insert PendingRpc
  -> serialize / post SEND
  -> return to caller
  ... CQ completion thread ...
  -> RdmaServer
  -> method lookup on one google::protobuf::Service*
  -> Service::CallMethod
  -> service done closure posts response SEND
  ... client RECV completion ...
  -> parse response / set RpcController
  -> erase PendingRpc / invoke done once
```

Stage 1 begins with `Probe`. The generated Stub and Service are the purpose of this stage, not a temporary shim to bypass.

## Connection Management and QP Topology

One Client–StorageNode pair uses one `RdmaConnection` with exactly one RC QP in the MVP. The same QP carries SEND/RECV control traffic, RDMA READ, RDMA WRITE, and READY/COMMIT SEND.

`librdmacm` owns connect/listen/accept and connection lifecycle:

```text
Client: create event channel/id -> resolve address -> ADDR_RESOLVED
        -> resolve route -> ROUTE_RESOLVED -> create PD/CQ/RC QP
        -> pre-post RECV -> connect -> ESTABLISHED

Server: create event channel/id -> bind -> listen -> CONNECT_REQUEST
        -> create connection context + PD/CQ/RC QP -> pre-post RECV
        -> accept -> ESTABLISHED
```

`libibverbs` owns PD, CQ, RC QP, MR, SEND, RECV, RDMA READ, RDMA WRITE, and completions. Cleanup-safe resource ordering may follow the implementation, but these responsibilities and the one-RC-QP topology are fixed.

## V1 Call Semantics

- The connection owns one RC QP; it does not create separate control, READ, WRITE, or READY QPs.
- `CallMethod()` allocates an atomic `request_id`, serializes the request, records `PendingRpc { request_id, controller, response, done }`, posts SEND, and returns without waiting for the response.
- A posted request’s SEND bytes remain valid through SEND completion. The caller keeps `controller`, `response`, and `done` valid through asynchronous completion.
- One ordinary per-connection completion thread uses `ibv_get_cq_event()`, re-registers notifications, drains `ibv_poll_cq()`, acknowledges events, and dispatches each CQE through `HandleCompletion()`.
- A lifetime-safe `WrContext { type, request_id, slot_id }` referenced by `wr_id` distinguishes SEND, RECV, READ, and WRITE. V1 does not bit-pack `wr_id` or use a shared operation registry.
- Client response completion parses into the pending response, records transport/framing/parse/service failure in the controller, erases the pending entry, and invokes `done` exactly once.
- A failure before any WR is posted may complete the closure inline; after successful posting, completion is driven by the CQ thread.
- Request/response control payloads and pending entries remain bounded by configuration; there is no request scheduler or multi-level queue.
- Unknown version, method, length, or request id fails/reconciles the affected call and connection as required.
- Teardown stops and joins the completion thread, then deterministically fails/completes remaining pending RPCs and releases WR contexts, buffers, and connection resources.

## Single-Service Server

`RdmaServer` is constructed with one `google::protobuf::Service*`. It consults that service’s descriptor to resolve a method and retains a minimal server RPC context containing request, response, controller, request id, completion closure, and temporary resources. `Service::CallMethod()` may return before the RPC completes. Only invocation of the service closure authorizes response serialization/posting. The server does not own a service registry, name-to-service map, plugin mechanism, or general asynchronous dispatcher.

## Control Methods by Stage

| Stage | Method | Purpose |
|---|---|---|
| 1 | `Probe` | Prove generated Stub/RpcChannel/server/service round trip |
| 2 | Pull write control | Carry chunk identity and client address/rkey/length; return durable store result |
| 3 | Initialize/fetch Client SlotPool | Return only this connection’s bounded slot descriptors |
| 3 | Ready/commit slot | Declare same-QP WRITE completion and request validation/durable publication |

Exact message names may follow project naming style, but these state transitions and responsibilities are fixed.

## Data Plane

Chunk bytes never appear in control Protobuf messages.

### Pull

1. Client allocates/registers an on-demand `RdmaMemoryRegion` and writes the chunk bytes into it.
2. Control request carries identity, checksum, length, address, rkey, and generation.
3. Server allocates/registers an on-demand destination region, posts RDMA READ, and returns from the service method without blocking its caller.
4. READ completion stabilizes/owns the bytes, validates them, calls the store bridge, and releases the server region only after the final byte consumer finishes.
5. After the durable store result, the service completion closure posts the response.
6. Client response completion runs `done`; only then may the client deregister/free its source region because the final response confirms server READ completion.

### Push V1

1. After connection establishment/initialization, the Store creates or assigns a fixed SlotPool exclusively to that Client and returns `slot_id`, `remote_addr`, `rkey`, `capacity`, `generation`, and `state`.
2. Client stores only those slots in its `RemoteSlotMap`, selects a local `FREE` slot, and marks it `WRITING` without a per-chunk ownership request.
3. Client posts `IBV_WR_RDMA_WRITE` on the connection’s RC QP and returns to the business caller.
4. WRITE CQ completion invokes the operation callback, which posts `IBV_WR_SEND` READY/COMMIT on the same RC QP with identity, checksum, length, pool generation, and slot generation.
5. Store validates that READY names a slot from this connection’s pool, transitions it through `READY` and `FLUSHING`, validates the bytes, and calls the Store adapter.
6. Store returns the durable result on the same connection and returns the slot to `FREE` only when reuse is safe.

RC QP ordering prevents READY from overtaking its WRITE. Neither WRITE completion nor READY is durable success; only the ACK after `ChunkStore::WriteChunk()` reports durability completes the chunk write.

## Resource Ownership

| Resource | Owner until |
|---|---|
| `PendingRpc` and caller pointers | Response completion removes the entry and runs `done`, or teardown completes it with failure |
| Request/response SEND buffer | Corresponding SEND completion |
| Client Pull temporary region | Final response confirms server READ completion, or safe teardown reconciliation |
| Server Pull temporary region | READ completion plus checksum/store byte consumption |
| Per-connection fixed RECV buffer | Its RECV completion is handled and the buffer is re-posted, or connection teardown proves no WR remains |
| Per-client Push SlotPool | Store; Client holds only a connection/generation-bound mirror of its own descriptors |
| Store input bytes | Store call returns and no asynchronous consumer remains |
| `WrContext` | Its CQE is handled |
| Connection resources/completion thread | Close completes after outstanding work is drained/failed safely and the thread joins |

## Explicit Exclusions

No separate control/READ/WRITE/READY QPs, independent READY connection, global SlotPool shared across Clients, cross-client CAS, per-chunk ownership RPC, generic `RdmaMemoryPool`, buffer cache, registered-MR reuse layer, shared CQ event loop/runtime, WR-id bit packing, request manager, task scheduler, priority scheduling, timeout wheel, automatic reconnect framework, selective signaling scheme, multi-service registry, SPDK/FastBlock poller, or general async runtime is part of this contract.
