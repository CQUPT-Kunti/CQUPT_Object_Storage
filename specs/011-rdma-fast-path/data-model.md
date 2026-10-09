# Data Model: RDMA Fast Path

All types below are transport/runtime state unless stated otherwise. They do not alter the persisted chunk, manifest, Metadata, Raft, or View formats.

## `RdmaRpcHeader`

Bounded framing metadata for a control request or response.

| Field | Meaning |
|---|---|
| `magic`, `version` | Reject incompatible frames |
| `request_id` | Correlate an asynchronous request and response; retained for diagnostics/idempotency |
| `method_index` or bounded method id | Select a method on the one registered service |
| `payload_length` | Bound parsing and receive buffer use |
| `status_code` | Transport/controller result in a response |

Validation: known version, known message kind, configured maximum payload, matching request id, and known method. Chunk bytes are never part of this frame.

## `RdmaRpcController`

A small concrete `google::protobuf::RpcController` used by the generated Stub and server dispatch.

State: `failed`, `error_text`, `canceled`. The completion thread updates failure state before invoking the caller’s closure. Cancellation callbacks and deadlines do not create a general asynchronous runtime in V1.

## `RdmaConnection`

Owns one Client–StorageNode transport connection, exactly one RC QP, and a minimal asynchronous RPC correlation table. The same QP carries SEND, RECV, RDMA READ, RDMA WRITE, and READY/COMMIT SEND.

Lifecycle: `DISCONNECTED → CONNECTING → READY → CLOSING → CLOSED`.

Minimal asynchronous members:

```cpp
std::thread completion_thread_;
std::atomic<bool> running_{false};
std::atomic<uint64_t> next_request_id_{1};
std::mutex pending_mutex_;
std::unordered_map<uint64_t, PendingRpc> pending_rpcs_;
```

`CallMethod()` records a `PendingRpc`, posts SEND, and returns. The completion thread waits through `ibv_comp_channel`, drains the CQ, handles CQEs, and invokes callbacks. Request/response buffers and associated WR contexts remain owned until their specific completion or deterministic connection teardown.

Linux connection-management fields are backed by `librdmacm` (`rdma_event_channel`, `rdma_cm_id`, CM events/lifecycle). PD, CQ, RC QP, MR, work requests, and completions are backed by `libibverbs`.

## `PendingRpc`

Minimal client correlation record:

```cpp
struct PendingRpc {
    uint64_t request_id;
    google::protobuf::RpcController* controller;
    google::protobuf::Message* response;
    google::protobuf::Closure* done;
};
```

It may additionally own the serialized request/SEND buffer and its cleanup handle so those bytes outlive SEND completion. The caller guarantees that `controller`, `response`, and `done` remain valid until asynchronous completion. On response RECV completion, the handler parses the response, records failure if needed, removes the map entry, and runs `done` exactly once.

## `WrContext`

```cpp
enum class WrType { Send, Recv, Read, Write };

struct WrContext {
    WrType type;
    uint64_t request_id;
    uint64_t slot_id;
};
```

`wr_id` references a lifetime-safe context. SEND completion releases only SEND-owned bytes/context; RECV locates a pending/client or server request; READ/WRITE triggers the operation-specific callback. The context remains valid until its CQE is handled. V1 does not use bit packing or a shared operation registry.

## `ServerRpcContext`

Minimal server-owned state holding the parsed request, response, controller, completion closure, request id, and any temporary RDMA resources. `Service::CallMethod()` may return before this context completes. The service closure serializes/posts the response only after asynchronous READ/WRITE/store work finishes; response SEND resources remain valid through SEND completion.

## `ConnectionGeneration`

A monotonically changing runtime token assigned when a connection becomes ready. Cached remote descriptors are valid only for their originating connection generation.

It is not persisted and is not a Raft term or storage generation.

## `RdmaMemoryRegion`

| Field | Meaning |
|---|---|
| `buffer_` | Buffer obtained on demand, for example with `malloc()` |
| `size_` | Registered byte length |
| `mr_` | `ibv_mr*` returned by `ibv_reg_mr()` |
| `state` | `ALLOCATED`, `REGISTERED`, `IN_FLIGHT`, `COMPLETED`, `DEREGISTERED`, or `FREED` |

Lifecycle:

```text
malloc -> ibv_reg_mr -> post WR -> asynchronous completion
       -> ibv_dereg_mr -> free
```

Cleanup is callback-driven and never requires the business thread to block. For Pull, the client region remains registered until the final response confirms the server completed RDMA READ. The server destination region remains registered through READ completion and remains allocated until checksum/store consumers finish.

There is no generic MemoryPool, buffer cache, registered-MR reuse, pool allocator, or dynamic pool growth in the MVP.

## Connection RECV Buffers

Each connection owns a small fixed set of registered RECV buffers. A RECV completion processes the message and re-posts the buffer; the MR is released only after the connection stops and no associated RECV WR remains outstanding. These buffers satisfy SEND/RECV protocol requirements and are not a generic memory pool.

## `RdmaDescriptor`

Control-plane description of a data region: address, rkey, length, connection generation, and chunk/request identity. Possession of a descriptor does not imply durable completion or long-lived validity.

## `RemoteSlot`

Server-owned Push buffer descriptor within one Client connection’s assigned SlotPool.

| Field | Meaning |
|---|---|
| `slot_id` | Stable index within the current pool generation |
| `addr`, `rkey` | Current remote-write descriptor |
| `capacity` | Maximum accepted chunk bytes |
| `generation` | Rejects stale descriptors after pool refresh/reconnect/restart |
| `state` | `FREE`, `WRITING`, `READY`, or `FLUSHING` |

Valid transition:

```text
FREE -> WRITING -> READY -> FLUSHING -> FREE
```

Before posting RDMA WRITE, the Client may return its local `WRITING` selection to `FREE`. After transfer starts, the slot returns to `FREE` only after completion/state reconciliation makes reuse safe. RDMA WRITE and READY use the same RC QP.

## `ClientSlotPool`

Created and registered by the Store for one established Client connection, then returned as a descriptor set during initialization/pool fetch. It is not shared with other Clients.

Fields: storage node id, client/connection identity, pool generation, connection generation, MR ownership, bounded slot descriptors, and lifecycle state.

The Store creates/destroys the pool, registers its MR, changes generations, and invalidates it on disconnect, reconnect, and Store restart. Normal writes do not acquire per-chunk ownership. READY validation requires the correct connection, pool generation, slot generation, acceptable data length, and valid slot state.

## `RemoteSlotMap` (Push base in Stage 3; lifecycle hardening in Stage 5)

Client-side mirror of its Store-assigned pools:

```text
StorageNodeId -> RemoteSlot[]
```

Each entry contains `slot_id`, `remote_addr`, `rkey`, `capacity`, `generation`, and `state`, plus its pool and connection generations. For a given `StorageNodeId`, entries belong only to the current Client. The Client selects a local `FREE` entry and marks it `WRITING`; no per-chunk ownership request is required. The mirror never overrides Store authority.

Invalidate an entry on:

- storage-node restart or server generation change;
- reconnect or connection generation change;
- returned slot/rkey generation change;
- explicit invalidation/rejection from the server.

Map miss or invalidation causes the Client to refresh its assigned pool from the Store. It does not acquire an individual slot for each chunk. The Store’s per-client SlotPool is always authoritative.

## `RdmaAttemptState`

| State | Terminal meaning | Next action |
|---|---|---|
| `not_started` | No remote data operation was posted | gRPC fallback allowed |
| `rejected_before_transfer` | Explicit pre-transfer rejection, including temporary per-client pool exhaustion, no suitable assigned slot capacity, remote-buffer shortage, or RDMA resource shortage | gRPC fallback allowed |
| `remote_state_uncertain` | Remote execution may have occurred | Reconcile using identity/idempotency; no fallback |
| `failed` | Definitive invalid metadata/protocol, system-size limit, checksum, identity conflict, or other non-retryable result | Return failure; no fallback |
| `durable_success` | `ChunkStore::WriteChunk()` returned durable success | Record replica success |

## `RdmaChunkWriteResult`

Control result mapped from the existing store contract: status, durable flag, already-exists flag, observed checksum, and attempt state. It must not fabricate durable success from a verbs completion.

## Persistent Data Impact

None. Chunk identity, on-disk chunk bytes, live index behavior, manifests, cleanup candidates, and Metadata/Raft state retain their existing formats and authorities.
