# Research and Settled Decisions: RDMA Fast Path

This document records decisions verified against the current repository. It replaces earlier proposals that deferred `RpcChannel`, placed RDMA under store, or treated client slot caching as an unspecified future idea.

## Decision 1: `RpcChannel` is the Stage 1 MVP

**Decision**: Implement a small asynchronous `RdmaConnection : google::protobuf::RpcChannel` immediately. `CallMethod()` assigns a request id, records a minimal pending entry, posts SEND, and returns; a per-connection CQ completion thread parses the response and invokes the generated Stub’s closure.

**Why**: The explicit learning objective is the whole Stub → `RpcChannel` → custom RDMA → Server → Service path. Deferring this behind a bespoke control API would miss that objective.

**Repository fact**: Current gRPC schemas do not set `option cc_generic_services = true`, so their C++ output does not provide the classic generic `google::protobuf::Service`/Stub pair required here.

**Schema/build choice**: Add `proto/rdma_control.proto`, enable `cc_generic_services`, run only the normal Protobuf C++ generator for it, and expose `rdma_control_proto`. Do not run the gRPC plugin for this schema. Root `proto/` and root CMake wiring follow the existing repository layout.

**Rejected**: Deferring `RpcChannel`; implementing a bespoke control client first; converting existing gRPC services; building a general RPC framework.

## Decision 2: Generic RDMA belongs in `modules/rdma/`

**Decision**: Put transport, connection, server, on-demand memory-region wrapper, and slot pool under `modules/rdma/`. Build them as `rdma_core` from the root CMake file.

**Why**: These are transport mechanisms, not storage policy. `rdma_core` can remain reusable and independent of `ChunkStore`, while a small store bridge maps a completed RDMA operation to existing storage semantics.

**Repository fact**: The current project has no module-local CMake files; all source lists and targets are wired at the root. Therefore no `modules/rdma/CMakeLists.txt` is introduced.

**Store seam**: `modules/store/node/storage_rdma_service.h/.cpp` owns descriptor-to-write-request conversion and delegation to `ChunkStore::WriteChunk()`.

**Build ownership**: The existing `storage_node_app` target links `rdma_core` for the server/adapter path, and `storage_client` links it for the client path. RDMA is not added as a Raft-core dependency.

**Rejected**: `modules/store/rdma/`; making `rdma_core` depend on store; moving durable publish into the transport module.

## Decision 3: Use librdmacm, libibverbs, and one RC QP

**Decision**: Linux V1 uses `librdmacm` for connect/listen/accept and connection lifecycle, and `libibverbs` for PD, CQ, RC QP, MR, SEND, RECV, RDMA READ, RDMA WRITE, and completions. One Client–StorageNode pair has one `RdmaConnection` and exactly one RC QP. That QP carries control and data operations; in Push, WRITE completion posts READY SEND on the same QP.

**Why**: This exposes the complete RDMA CM and verbs lifecycle while avoiding separate control/READ/WRITE/READY QPs. RC QP ordering gives the MVP a simple WRITE→READY relationship without another network connection.

**Client CM flow**: create event channel/id, resolve address, wait for `ADDR_RESOLVED`, resolve route, wait for `ROUTE_RESOLVED`, create PD/CQ/RC QP, pre-post RECV, connect, then wait for `ESTABLISHED`.

**Server CM flow**: create event channel/id, bind, listen, accept `CONNECT_REQUEST`, create per-connection context plus PD/CQ/RC QP, pre-post RECV, accept, then observe `ESTABLISHED`.

**Execution model**: V1 uses one ordinary CQ completion thread per Client or Server connection. `CallMethod()` does not wait for a response. A mutex-protected pending map correlates responses, while lifetime-safe `WrContext` objects distinguish SEND, RECV, READ, and WRITE completions through `wr_id`.

**Server dispatch**:

1. Receive a bounded `RdmaRpcHeader` and request bytes.
2. Match the method against the single service descriptor.
3. Allocate the generated request/response prototypes.
4. Parse and call `Service::CallMethod()` with a completion closure while retaining the request/response/controller context.
5. When the service eventually runs the closure, serialize/post the response; release the response buffer only after SEND completion and release the RPC context when no asynchronous user remains.

**Rejected**: Separate operation-specific QPs, an independent READY connection, service registry/map, shared CQ dispatcher/runtime, WR-id bit packing, request manager, task scheduler, priority scheduling, timeout wheel, automatic reconnect framework, selective signaling machinery, SPDK/FastBlock poller integration, or a general async runtime.

## Decision 4: Pull first, per-client fixed-slot Push second

**Decision**: Stage 2 implements asynchronous Pull with on-demand client/server memory regions retained through completion. Stage 3 implements asynchronous Push with one fixed, bounded SlotPool per Client connection. The Store allocates/registers that pool during connection initialization; the Client receives only its own descriptors and does not request ownership for every chunk.

**Why**: Pull has the smallest server memory-management surface and proves large-payload separation from the control plane. Per-client Push pools add bounded server ownership without global slot competition, cross-client CAS, or dynamic per-request MR allocation.

**Durability boundary**: In both modes, RDMA completion only proves transport completion. The store bridge validates size/checksum/identity and calls `ChunkStore::WriteChunk()`. Only its durable result can produce a durable ACK.

**Rejected**: A global SlotPool shared by clients, `AcquireSlot` ownership negotiation for every chunk, cross-client CAS, putting chunk bytes in Protobuf, blocking the business thread for RDMA completion, or treating CQ completion/READY as durable success.

## Decision 5: Use on-demand `RdmaMemoryRegion`, not a generic pool

**Decision**: Ordinary operation buffers use a small `RdmaMemoryRegion` wrapper: allocate, `ibv_reg_mr`, post the asynchronous WR, and deregister/free from the completion or final-response cleanup path after the last use. No generic `RdmaMemoryPool`, buffer cache, registered-MR reuse, pool allocator, or dynamic pool growth is required.

**Why**: This feature is an RDMA workflow demo. Per-operation registration is simpler to reason about and makes ownership/lifetime visible; optimizing registration costs is outside the MVP.

**Required exceptions**: Each connection retains a small fixed set of registered RECV buffers because SEND requires pre-posted receives. Per-client Push SlotPool MRs also remain registered while the exported `addr/rkey` values are valid. Neither is a general reusable memory pool.

**Rejected**: Building a general MR cache/pool before profiling, releasing a region before its WR/data consumer completes, or deleting the protocol-required Push SlotPool.

## Decision 6: `RemoteSlotMap` starts with Push and is hardened in Stage 5

**Decision**: Stage 3 creates the minimal connection-scoped mirror needed by Push. Stage 5 hardens it as `StorageNodeId -> RemoteSlot[]`, where each entry carries `slot_id`, `remote_addr`, `rkey`, `capacity`, `generation`, and `state`. For each storage node, the map contains only the current Client’s assigned SlotPool.

**Why**: Normal Push writes can select a locally `FREE` slot and avoid a per-chunk ownership RPC. The map is still only a local mirror; the Store remains authoritative for pool creation/destruction, MR registration, generation, disconnect, reconnect, and restart invalidation.

**Invalidation events**: store restart, reconnect, connection generation change, slot/rkey generation change, and explicit server invalidation.

**Rejected**: Mirroring another Client’s slots, treating the mirror as Store authority, retaining entries across invalidating lifecycle events, or leaving slot mirroring as a generic “future optimization.”

## Decision 7: Fallback is state-based, not error-string-based

**Decision**: Every attempt ends in one of five states:

| State | Meaning | gRPC fallback |
|---|---|---|
| `not_started` | No remote data operation began | Allowed |
| `rejected_before_transfer` | Server definitively rejected before data movement, including temporary per-client pool/slot/buffer/RDMA-resource shortage | Allowed |
| `remote_state_uncertain` | Data or request may have executed remotely | Forbidden; reconcile |
| `failed` | Definitive invalid metadata/protocol, system-size limit, checksum, or identity-conflict result | Forbidden |
| `durable_success` | Existing durable write contract completed | Not needed |

**Why**: Network errors after a remote operation starts cannot prove non-execution. Identity and idempotency rules must reconcile those cases.

An unsuitable available slot capacity is fallback-safe only when detected before posting RDMA READ/WRITE. A chunk above the system’s allowed maximum is a business/protocol failure and cannot be hidden by gRPC fallback.

## Decision 8: Endpoint offset is only an MVP bridge

**Decision**: Initially derive the RDMA endpoint from the discovered host plus a configured port offset.

**Why**: It avoids changing View, Metadata, or Raft during the transport experiment.

**Future direction**: Discovery may later advertise `grpc_endpoint`, `rdma_endpoint`, and `rdma_supported`. That is a separate contract evolution, not a V1 dependency.

## Decision 9: Use a minimal validation ladder

**Decision**: Add one focused contract test per stage and run only the smallest relevant existing regressions before advancing. Finish with low-parallel unit and persistence groups.

**Why**: Stage-level evidence isolates failures without front-loading a large test framework. Benchmarks and stress tests do not establish the required correctness boundaries and are excluded from this feature.

## Remaining Tuning Decisions

- Default RDMA port offset and collision policy.
- Maximum temporary MR size, Push slot count, and slot capacity for supported hardware.
- The release milestone at which explicit discovery capabilities replace endpoint derivation.
