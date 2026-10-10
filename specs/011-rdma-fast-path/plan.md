# Implementation Plan: RDMA Fast Path

**Branch**: `011-rdma-fast-path` | **Spec**: `specs/011-rdma-fast-path/spec.md`
**Planning mode**: amend the existing feature; do not create a new feature or write implementation code during this planning pass.

## Summary

Add a small optional `modules/rdma/` library and a store bridge. Stage 1 intentionally implements an asynchronous `RdmaConnection : google::protobuf::RpcChannel`, one CQ completion thread per connection, and a one-service `RdmaServer` to demonstrate the complete generated Protobuf callback chain. Stages 2 and 3 add asynchronous Pull and fixed-slot Push data transfer. Stage 4 selects RDMA as the preferred write path with fallback only before remote data transfer. Stage 5 adds the client `RemoteSlotMap`. Existing gRPC, `ChunkStore::WriteChunk()`, and `CommitObject` remain the durability and visibility authorities.

## Technical Context

- **Language/build**: C++20, root `CMakeLists.txt`, CMake presets, Ninja.
- **Existing RPC/schema**: Protobuf and gRPC generated from root `proto/` schemas.
- **RDMA backend**: Linux `librdmacm` for connection management plus `libibverbs` for verbs resources/data operations; portable unavailable implementation elsewhere.
- **Tests**: GoogleTest/CTest, one focused contract test per stage plus existing regressions.
- **Constraints**: bounded resources, minimal asynchronous V1, no generic event-loop/runtime framework, no persistence-format or public-behavior change.

## Current Baseline Reconciled with Source

- `StorageTransferClient::WriteChunk()` is the existing write abstraction; its gRPC implementation validates identity/size and requests publish durability.
- Object upload obtains targets from View, writes replicas through a bounded executor, records only durable results, and then calls Metadata `CommitObject`.
- `StorageNodeService` delegates writes to `ChunkStore::WriteChunk()`.
- `LocalDiskChunkStore::WriteChunk()` owns checksum/conflict checks, staging, required flush, publish, directory sync, live-index update, and idempotent duplicate behavior.
- Current `.proto` services generate gRPC interfaces but do not enable classic generic C++ `Service`/Stub generation.
- The repository uses root-level Protobuf generation and root-level target wiring; module-local `CMakeLists.txt` files are not the current convention.
- Existing execution is bounded and simple; no shared CQ or generic async framework exists.

## Settled Architecture Decisions

### Control schema and generated Stub

Create `proto/rdma_control.proto` with `option cc_generic_services = true`. Generate only its normal C++ Protobuf output (`--cpp_out`), not gRPC output, and expose it through a small `rdma_control_proto` target. Keeping the schema in root `proto/` matches the current repository and generation layout.

The initial service has a tiny `Probe` method. Later messages add descriptors and lifecycle controls for Pull and Push. Payload bytes never enter these messages.

### Generic module boundary

Create `modules/rdma/`:

```text
modules/rdma/
├── AGENTS.md
├── module-notes.md
├── rdma_connection.h/.cpp
├── rdma_server.h/.cpp
├── rdma_transport.h/.cpp
├── rdma_transport_linux.cpp
├── rdma_memory_region.h/.cpp
└── rdma_slot_pool.h/.cpp
```

The root build creates `rdma_core`; there is no module-local CMake file. `rdma_core` depends on Protobuf and `rdma_proto`, and with `CQUPT_RDMA=ON` it links `librdmacm` plus `libibverbs` directly, but not on store code. Store integration lives in `modules/store/node/storage_rdma_service.h/.cpp`. The existing `storage_node_app` target links `rdma_core` for the server/adapter path, and the existing `storage_client` target links it for the client path; the Raft core does not gain an RDMA dependency.

`modules/rdma/` owns only generic connection, QP, on-demand MR, SEND/RECV, READ/WRITE, completion, and SlotPool infrastructure. `modules/store/node/storage_rdma_service.*` remains the Storage/RDMA adapter that understands `ChunkStore` and converts completed RDMA input into the existing storage write contract.

Header changes are limited to new interfaces/types and the smallest integration declaration needed by existing transfer code. Complex flow, system calls, CQ processing, durable publish, and helpers remain in `.cpp` files.

### Minimal asynchronous execution model

- Client: for each Client–StorageNode pair, one `RdmaConnection`, exactly one RC QP, one completion thread, an atomic request-id source, and a mutex-protected `pending_rpcs_` map.
- `CallMethod()` serializes, creates `PendingRpc`, posts SEND, and immediately returns. SEND buffers remain owned through SEND completion; caller-owned response/controller/done objects remain valid until callback.
- Each WR uses a lifetime-safe `WrContext { type, request_id, slot_id }` referenced by `wr_id` so the completion handler can distinguish SEND, RECV, READ, and WRITE without bit packing or an operation registry.
- The completion thread uses `ibv_comp_channel`, `ibv_get_cq_event()`, notification re-registration, CQ draining via `ibv_poll_cq()`, event acknowledgement, and `HandleCompletion()`.
- Client response completion parses into the registered response, updates the controller, removes the pending entry, and invokes `done` exactly once.
- Server: each connection uses the same simple completion-thread pattern and one `google::protobuf::Service*`. It retains request/response/controller/closure context after `Service::CallMethod()` returns; the service closure serializes/posts the response only when the asynchronous operation actually completes.
- No request manager, task scheduler, shared CQ runtime, priority scheduling, timeout wheel, automatic reconnect framework, selective signaling policy, or new general-purpose worker runtime.
- The same RC QP carries control SEND/RECV, RDMA READ, RDMA WRITE, and READY; V1 does not split these operations across QPs or connections.

### Connection management and verbs responsibilities

`librdmacm` owns address/route resolution, listen/connect/accept, CM events, and connection teardown. `libibverbs` owns PD, CQ, RC QP, MR, SEND, RECV, RDMA READ, RDMA WRITE, and completion handling.

Client connection sequence:

```text
rdma_create_event_channel -> rdma_create_id -> rdma_resolve_addr
-> RDMA_CM_EVENT_ADDR_RESOLVED -> rdma_resolve_route
-> RDMA_CM_EVENT_ROUTE_RESOLVED -> create PD/CQ/RC QP
-> pre-post RECV -> rdma_connect -> RDMA_CM_EVENT_ESTABLISHED
```

Server connection sequence:

```text
rdma_create_event_channel -> rdma_create_id -> rdma_bind_addr -> rdma_listen
-> RDMA_CM_EVENT_CONNECT_REQUEST -> create connection context
-> create PD/CQ/RC QP -> pre-post RECV -> rdma_accept
-> RDMA_CM_EVENT_ESTABLISHED
```

The exact cleanup-safe ordering may be refined in code, but these library responsibilities and the single-RC-QP topology are fixed.

### Control and data planes

```text
generated control Stub
        │ CallMethod
        ▼
RdmaConnection ── post SEND; return ──> RdmaServer ──> one Service*
        │                                      │
 completion thread                     async done closure
        │                                      │
        └──── on-demand MR / Push slot ────────┘
                                               │
                                               ▼
                                  ChunkStore::WriteChunk()
                                               │ durable
                                               ▼
                                 response CQ -> client done
```

Pull allocates/registers temporary client and server `RdmaMemoryRegion` objects on demand. The server posts RDMA READ and returns from the business method; READ completion validates/persists the bytes and runs the service closure. The client releases its source MR only after the final response confirms READ completion. Push V1 gives every Client connection its own fixed, bounded SlotPool. The Client mirrors only that pool in `RemoteSlotMap`, chooses a local `FREE` slot, marks it `WRITING`, posts RDMA WRITE, and returns. WRITE completion posts READY/COMMIT as SEND on the same RC QP. The Store validates connection/pool/slot generation, persists through the adapter, ACKs, and returns the slot to `FREE`. There is no global pool, cross-client CAS, or normal per-chunk ownership request. In both modes, transport completion and READY are not durable completion.

### Memory registration model

The MVP has no generic `RdmaMemoryPool`, buffer cache, reusable MR allocator, or dynamic pool growth. Ordinary Pull and control-operation scratch buffers use `RdmaMemoryRegion`: allocate, register, post the asynchronous WR, then deregister/free from the completion/response cleanup path after the last user finishes. Each connection still owns a small fixed set of registered RECV buffers that are re-posted after processing. Push SlotPool MRs remain registered while their published `addr/rkey` descriptors are valid; they are protocol state, not a generic memory pool.

### Integration and fallback

RDMA is inserted behind the storage write abstraction. Existing discovery, replica accounting, manifest handling, cleanup candidates, Metadata commit, reads, Raft, and View behavior do not change. The MVP obtains the RDMA endpoint from the existing host plus a configured offset; a later discovery capability may advertise `grpc_endpoint`, `rdma_endpoint`, and `rdma_supported` without being part of V1.

Fallback to gRPC is permitted only for `not_started` and `rejected_before_transfer`. Temporary exhaustion of the current Client’s pool, no suitable slot capacity, remote-buffer shortage, or RDMA resource shortage is `rejected_before_transfer` only when the Store definitively rejects it before RDMA READ/WRITE begins. `remote_state_uncertain` requires reconciliation using the same identity/request semantics. Invalid metadata/protocol fields, system-limit violations, checksum failure, or identity conflict are `failed`. `durable_success` is terminal success.

## Delivery Stages

### Stage 0 — Build Boundary

- Add the `CQUPT_RDMA` ON/OFF option (default OFF); when ON, `rdma_core` links `librdmacm` and `libibverbs` directly without dependency detection.
- Add `rdma_proto`, `rdma_core`, module documentation, and the focused test target.
- Prove OFF builds without RDMA libraries and ON builds/links against `librdmacm` and `libibverbs`.

### Stage 1 — Minimal Async RpcChannel + RDMA Connection

- Add `Probe`, `RdmaRpcController`, asynchronous `RdmaConnection::CallMethod()`, minimal `PendingRpc`/`WrContext`, per-connection CQ completion threads, `librdmacm` client/server lifecycle, one RC QP, and asynchronous one-service `RdmaServer` dispatch.
- Test immediate return after posting, unavailable/setup failure, CM connect/accept, the single-QP topology, request-id matching, generated Stub callback traversal, controller status, callback-once behavior, and cleanup.

### Stage 2 — Pull

- Add on-demand `RdmaMemoryRegion` allocation/registration and descriptor exchange; do not add a generic registered-memory pool.
- Server posts RDMA READ and returns; READ completion validates, delegates to the store bridge, runs the response closure after the durable result, and triggers eventual MR cleanup.
- Test large chunk, asynchronous completion, checksum rejection, client/server temporary-MR lifetime, durable boundary, and cleanup.

### Stage 3 — Push V1

- Allocate one fixed, bounded SlotPool per Client connection and expose its descriptors during connection initialization/pool fetch.
- Enforce `FREE → WRITING → READY → FLUSHING → FREE`, connection/pool/slot generation, and asynchronous same-RC-QP WRITE-completion→READY ordering without per-chunk ownership acquisition.
- Test cross-client isolation, stale generation rejection, successful publish/ACK, and safe reuse.

### Stage 4 — Preferred RDMA + Safe Fallback

- Add transport selection behind `StorageTransferClient` and wire application configuration.
- Prefer configured RDMA mode; fall back only before remote data transfer.
- Test the complete attempt-state matrix and unchanged gRPC behavior.

### Stage 5 — Client Slot Map

- Harden `StorageNodeId -> RemoteSlot[]` as the local mirror of only the current Client’s assigned pool; normal writes reuse it without requesting ownership per chunk.
- Invalidate on store restart, reconnect, connection generation change, slot/rkey generation change, and explicit invalidation.
- Test reuse, invalidation, and pool refresh. The Store remains authoritative for pool creation/destruction, MR registration, generation, disconnect/reconnect, and restart invalidation.

## Validation Strategy

Each stage completes only its focused test and the smallest relevant existing regression before the next begins. The final pass runs the normal unit and persistence groups at low parallelism. No benchmark or stress gate is required.

| Stage | Focused evidence |
|---|---|
| 0 | OFF builds without RDMA libraries; ON builds and links `librdmacm`/`libibverbs` |
| 1 | CM lifecycle, one RC QP, immediate `CallMethod()` return, pending-map/CQ callback round trip, async server completion, and cleanup |
| 2 | Async Pull checksum, on-demand temporary-MR lifetime/cleanup, durable ACK |
| 3 | Per-client pool isolation, async same-QP WRITE-completion→READY, generation, durable ACK/reuse |
| 4 | Fallback state matrix and gRPC regression |
| 5 | Assigned-pool mirror reuse and every invalidation trigger |

## Constitution Check

- Existing business, protocol, persistence, and public API behavior remain unchanged.
- Required durability never silently degrades; durable success still comes only from `ChunkStore::WriteChunk()`.
- Complex behavior and platform code remain in `.cpp` files.
- New headers exist only for new module interfaces and minimal integration seams.
- The plan adds no test skips, deleted tests, SPDK/FastBlock dependency, or speculative runtime abstraction.

## Project Structure Changes

```text
proto/rdma_control.proto
modules/rdma/{AGENTS.md,module-notes.md,rdma_*.h,rdma_*.cpp}
modules/store/node/storage_rdma_service.h
modules/store/node/storage_rdma_service.cpp
tests/rdma_transport_contract_test.cpp
CMakeLists.txt
tests/CMakeLists.txt
```

Existing storage transfer and app files change only when Stage 4 selects the new path. No View, Metadata, Raft, read-path, or persisted-format file is changed.
