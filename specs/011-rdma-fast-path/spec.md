# Feature Specification: RDMA Fast Path

**Feature Branch**: `011-rdma-fast-path`  
**Status**: Design settled; implementation not started
**Scope**: Add an optional RDMA write fast path beside the existing gRPC storage path without changing object visibility, durability, identity, checksum, or retry semantics.

## Problem Statement

Large chunk payloads currently travel through the gRPC storage write path. The project needs a small, inspectable RDMA path that demonstrates the complete generated Protobuf Stub → `google::protobuf::RpcChannel` → RDMA transport → server → `google::protobuf::Service` chain and then grows into Pull and Push data transfer. RDMA is an optimization only: the existing gRPC path remains authoritative and available.

## User Scenarios and Independent Validation

### User Story 1 — Minimal asynchronous RpcChannel chain (Priority: P1)

As a developer, I can invoke a generated generic Protobuf Stub over one RDMA connection and receive a decoded response, so the control path is understood end to end before chunk transfer is added.

**Independent test**: A generated `Probe` Stub call makes `RdmaConnection::CallMethod()` return after posting the request, then a CQ completion thread matches the response by `request_id`, parses it, updates the controller, invokes `done` exactly once, and releases all request resources.

### User Story 2 — Pull-mode durable write (Priority: P2)

As an uploader, I can expose a registered client buffer and ask the storage node to RDMA READ a chunk, validate it, and durably publish it through the existing `ChunkStore::WriteChunk()` boundary.

**Independent test**: A large chunk uses an on-demand registered client MR, the server handles RDMA READ completion asynchronously, identity and checksum are verified, and the temporary MRs are released only after their asynchronous use ends; success is returned only after `WriteChunk()` reports durable success.

### User Story 3 — Push-mode durable write (Priority: P3)

As an uploader, I can use a slot from the bounded SlotPool that the storage node assigned exclusively to my connection, RDMA WRITE into it, mark it ready, and receive a durable acknowledgement after the storage node validates and publishes the chunk.

**Independent test**: A client uses only its own pool, and a slot completes `FREE → WRITING → READY → FLUSHING → FREE`; asynchronous WRITE completion posts READY SEND on the same RC QP, generation checks reject stale use, and the durable ACK is emitted only after `WriteChunk()` succeeds.

### User Story 4 — Preferred RDMA with safe fallback (Priority: P4)

As an uploader, I can prefer RDMA while preserving exactly-once-visible behavior when RDMA is unavailable or rejects the operation before remote data transfer starts.

**Independent test**: The fallback matrix permits gRPC only for `not_started` and `rejected_before_transfer`, and forbids fallback for `remote_state_uncertain`, `failed`, and `durable_success`.

### User Story 5 — Client RemoteSlotMap (Priority: P5)

As a Push-mode client, I can mirror the slots assigned exclusively to my connection by each storage node, select a local `FREE` slot without requesting ownership for every chunk, and still treat the storage node as final authority.

**Independent test**: The map contains only the current client’s assigned slots, reuses them only within the same connection/pool generation, and is invalidated by reconnect, store restart, generation/rkey changes, or explicit invalidation.

## Functional Requirements

- **FR-001**: The existing gRPC storage path MUST remain functional and be the only fallback path.
- **FR-002**: RDMA MUST be optional at configure time via a single `CQUPT_RDMA` ON/OFF switch (default OFF); when enabled, missing `rdmacm`/`ibverbs` MUST fail configure, compile, or link explicitly rather than silently degrading. It MUST NOT silently claim RDMA durability or transfer support.
- **FR-003**: Control messages MUST remain Protobuf messages. A small control schema MUST generate the classic C++ `Service` and Stub interfaces needed by `google::protobuf::RpcChannel`.
- **FR-004**: Stage 1 MUST provide an asynchronous `RdmaConnection : google::protobuf::RpcChannel`. In the MVP, one Client–StorageNode pair uses one `RdmaConnection`, which owns exactly one RC QP and a simple CQ completion thread.
- **FR-005**: `RdmaConnection::CallMethod()` MUST allocate a `request_id`, serialize the request, create and register a minimal `PendingRpc`, post SEND on the RC QP, and return without waiting for the network response. The completion thread MUST match the response, parse it, update `RpcController`, remove the pending entry, and invoke `done` exactly once.
- **FR-006**: The first server MUST hold exactly one `google::protobuf::Service*`; it MUST dispatch by method descriptor without a registry, plugin system, or general asynchronous RPC runtime. It MUST retain request/response/controller state until the service completion closure runs and MUST NOT equate `Service::CallMethod()` return with RPC completion.
- **FR-007**: Chunk bytes MUST NOT be embedded in Protobuf control messages. Control messages may carry identity, checksum, length, address/rkey, slot, generation, and completion state.
- **FR-008**: Pull mode MUST allocate/register temporary `RdmaMemoryRegion` objects on demand. The client MR MUST remain valid until the final response proves the server completed RDMA READ, and the server’s temporary destination region MUST remain valid through READ completion and subsequent byte use before deregistration/free. The server MUST validate identity, size, and checksum before durable publication.
- **FR-009**: Push V1 MUST use a fixed, bounded SlotPool assigned by the storage node to each Client connection. A Client MUST see and use only its own pool; normal chunk writes select a local `FREE` slot without a per-chunk ownership request. READY MUST validate connection/pool/slot generation before durable publication.
- **FR-010**: RDMA completion MUST mean only transport completion. Durable success MUST be reported only after the existing `ChunkStore::WriteChunk()` contract succeeds.
- **FR-011**: The implementation MUST preserve the existing chunk identity, checksum, idempotency, conflict detection, staged publish, manifest, `CommitObject`, read, Metadata, Raft, and View semantics.
- **FR-012**: `CommitObject` MUST remain the object visibility boundary; RDMA MUST NOT bypass the existing upload orchestration or replica-success accounting.
- **FR-013**: A retry or gRPC fallback MUST be selected from explicit terminal states: `not_started`, `rejected_before_transfer`, `remote_state_uncertain`, `failed`, and `durable_success`.
- **FR-014**: gRPC fallback MUST be allowed only before remote data transfer starts. Uncertain remote state MUST be reconciled by identity/idempotency rules, never by blind fallback.
- **FR-015**: Stage 5 MUST harden the client `StorageNodeId -> RemoteSlot[]` mirror, including `slot_id`, `remote_addr`, `rkey`, `capacity`, `generation`, and `state`. It MUST contain only that Client’s assigned pool and MUST NOT replace storage-node authority over pool creation, destruction, MR registration, generation, disconnect, reconnect, or restart invalidation.
- **FR-016**: The MVP endpoint rule MAY derive RDMA from the existing host and a configured port offset. This rule MUST be documented as temporary and MUST NOT require changes to View, Metadata, or Raft contracts.
- **FR-017**: The first implementation MUST use a simple asynchronous model: `request_id`, a mutex-protected pending map, one ordinary CQ completion thread per connection, `WrContext` values, and callback/Closure completion. It MUST NOT introduce SPDK/FastBlock pollers, a shared CQ runtime, request manager, task scheduler, priority queue, timeout wheel, automatic reconnect framework, or general async runtime.
- **FR-018**: The generic RDMA module MUST live at `modules/rdma/` and remain independent of store semantics. Store-specific conversion to `ChunkStore::WriteChunk()` MUST live in the store layer.
- **FR-019**: Shutdown and error paths MUST stop/join completion threads and release pending RPCs, WR contexts, MRs, per-client SlotPools, buffers, RC QPs, and connections deterministically. Request SEND buffers MUST remain valid until SEND completion; caller-owned response/controller/done objects MUST outlive asynchronous completion.
- **FR-020**: The Linux implementation MUST use `librdmacm` for connect/listen/accept and connection lifecycle, and `libibverbs` for PD, CQ, RC QP, MR, SEND, RECV, RDMA READ, RDMA WRITE, and completions.
- **FR-021**: Push MUST post RDMA WRITE and return without blocking the business thread. Its WRITE completion callback MUST post the subsequent READY/COMMIT SEND through the same `RdmaConnection` and RC QP. READY and RDMA completion MUST NOT be treated as durable success.
- **FR-022**: Temporary slot exhaustion, unsuitable available slot capacity, remote buffer shortage, or RDMA resource shortage explicitly rejected before any remote data operation MUST map to `rejected_before_transfer`; invalid metadata, identity conflict, invalid checksum fields, protocol violations, or chunks above the system maximum MUST map to `failed`.
- **FR-023**: Each posted SEND, RECV, READ, or WRITE MUST have a lifetime-safe `WrContext` containing its operation type and correlation fields; `wr_id` MUST reference that context without bit packing or a complex operation registry.
- **FR-024**: The MVP MUST NOT require a generic `RdmaMemoryPool`, registered-buffer cache, reusable MR allocator, or dynamic pool growth. Ordinary operation buffers MUST use on-demand `malloc`/registration and callback-driven deregistration/free. Small pre-posted per-connection RECV buffers and long-lived Push SlotPool MRs remain required protocol resources, not a generic memory pool.

## Non-Goals

- Replacing gRPC, Protobuf, View discovery, Metadata, Raft, or `CommitObject`.
- Moving read traffic to RDMA in this feature.
- A multi-service registry or production-grade generic RPC framework.
- Industrial RPC multiplexing, shared CQ dispatch, selective signaling, multi-level request queues, priority scheduling, or a new thread-pool/runtime abstraction.
- Separate control/READ/WRITE/READY QPs, a global SlotPool shared by clients, cross-client slot CAS, or per-chunk ownership acquisition in the normal Push path.
- Changing persisted chunk format, public storage API behavior, or object visibility rules.
- Performance benchmarks or stress suites as stage gates.

## Key Entities

- **RdmaConnection**: asynchronous generic Protobuf channel, one RC QP, one completion thread, and a minimal pending map for one Client–StorageNode pair.
- **RdmaServer**: receive/dispatch/respond loop for one `google::protobuf::Service*`.
- **RdmaRpcHeader**: bounded framing metadata for method, request/response length, request id, and status.
- **PendingRpc**: correlation record retaining caller pointers and any temporary request resources until response completion.
- **WrContext**: lifetime-safe SEND/RECV/READ/WRITE completion discriminator referenced by `wr_id`.
- **RdmaMemoryRegion**: one on-demand allocation/registration whose callback-driven cleanup occurs after asynchronous use ends.
- **ClientSlotPool / RemoteSlot**: storage-node-owned, connection-specific Push pool and its generation-bound slot descriptors.
- **RemoteSlotMap**: client-side mirror of only that Client’s assigned slots, keyed by `StorageNodeId`.
- **RdmaAttemptState**: the transport-selection state used to decide retry, reconciliation, or safe fallback.

## Success Criteria

- **SC-001**: One targeted Stage 1 test proves that `CallMethod()` returns before response completion and that the generated Stub → pending map → CQ thread → Server/Service → response → `done` chain completes exactly once with cleanup.
- **SC-002**: One targeted Pull test proves asynchronous large-payload RDMA READ, checksum validation, on-demand client/server MR lifetime and cleanup, and durable acknowledgement.
- **SC-003**: One targeted Push test proves per-client pool isolation, same-RC-QP WRITE→READY ordering, generation/state transitions, durable publication, acknowledgement, and reuse.
- **SC-004**: One targeted fallback test covers every terminal state and demonstrates that no fallback occurs after uncertain remote execution.
- **SC-005**: One targeted map test proves that only the current Client’s pool is mirrored, normal writes do not request ownership per chunk, and every required invalidation event is enforced.
- **SC-006**: With `CQUPT_RDMA=OFF`, existing unit and persistence tests continue to pass through gRPC.

## Assumptions

- Linux with `librdmacm` and `libibverbs` is the first real RDMA platform.
- Pull is the first chunk-transfer mode and the default RDMA mode once enabled; Push becomes selectable after Stage 3.
- Each Client and Server connection may use one ordinary CQ completion thread because the objective is correctness and learning, not throughput.
- Exact port offset, maximum temporary MR size, and Push slot counts are deployment tuning values rather than protocol semantics.
