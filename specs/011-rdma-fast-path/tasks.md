# Tasks: RDMA Fast Path

**Input**: `spec.md`, `plan.md`, `research.md`, `data-model.md`, and `contracts/`
**Execution rule**: Finish and validate one stage before starting the next. Tests are written before the implementation they specify. `[P]` means the task can run in parallel with other `[P]` tasks in the same stage when files do not overlap.

## Stage 0 — Build Boundary

- [ ] T001 Add `proto/rdma_control.proto` with `option cc_generic_services = true` and initial `Probe` messages/service; update root `CMakeLists.txt` to generate normal C++ Protobuf output only as `rdma_control_proto`.
- [ ] T002 [P] Add `modules/rdma/AGENTS.md` and `modules/rdma/module-notes.md` documenting minimal asynchronous V1, per-connection completion-thread ownership, on-demand `RdmaMemoryRegion`, Push SlotPool/RECV-buffer exceptions, and the store-dependency prohibition.
- [ ] T003 Add `CQUPT_RDMA={OFF,AUTO,REQUIRED}`, the root-wired `rdma_core` target with Linux `librdmacm` and `libibverbs` detection, portable unavailable backend in `modules/rdma/rdma_transport.{h,cpp}`, Linux source selection, and `rdma_transport_contract_test` in root/test CMake files; validate OFF/AUTO build and clear REQUIRED failure when either dependency is unsupported.

**Stage gate**: Build boundary works without RDMA hardware and has no module-local CMake file.

## Stage 1 — User Story 1: Minimal Async RpcChannel + RDMA Connection (P1)

**Goal**: Generated Stub → asynchronous `RpcChannel` → pending map/CQ completion → asynchronous one-service server → generated response callback.

- [ ] T004 [US1] Write Stage 1 cases in `tests/rdma_transport_contract_test.cpp` for `CallMethod()` returning before response, request-id correlation, SEND-buffer lifetime, CQ callback completion, asynchronous server closure, controller error, `done` exactly once, unavailable mode, one RC QP, malformed frame rejection, and deterministic shutdown cleanup.
- [ ] T005 [US1] Implement the smallest `librdmacm` client/server flows plus per-connection `ibv_comp_channel`/CQ completion thread and lifetime-safe SEND/RECV `WrContext` handling in `modules/rdma/rdma_transport_linux.cpp`, including notification re-registration, CQ draining, event acknowledgement, pre-posted/re-posted fixed RECV buffers, and cleanup-safe stop/join; keep portable unavailable behavior in `rdma_transport.cpp`.
- [ ] T006 [US1] Implement `RdmaRpcController`, atomic request ids, minimal `PendingRpc`, mutex-protected `pending_rpcs_`, and asynchronous `RdmaConnection : google::protobuf::RpcChannel` in `modules/rdma/rdma_connection.h/.cpp`; post SEND then return, parse response on RECV completion, remove the entry, update the controller, run `done` exactly once, and retain request resources through SEND completion.
- [ ] T007 [US1] Implement `RdmaServer` in `modules/rdma/rdma_server.h/.cpp` with one `google::protobuf::Service*`, descriptor-based method lookup, retained server request/response/controller/closure context after `Service::CallMethod()` returns, asynchronous response posting from the service closure, and no registry or general async runtime.

**Stage gate**: T004 passes and demonstrates immediate caller return plus the complete asynchronous generated call/callback chain.

## Stage 2 — User Story 2: Pull Durable Write (P2)

**Goal**: Server RDMA READs a client buffer and acknowledges only the existing durable store boundary.

- [ ] T008 [US2] Extend `tests/rdma_transport_contract_test.cpp` with Pull cases for asynchronous READ completion, large payload separation, checksum mismatch, on-demand client/server MR lifetime and callback cleanup, disconnect cleanup, idempotent duplicate, and durable ACK ordering.
- [ ] T009 [US2] Implement on-demand allocate/register/deregister/free ownership in `modules/rdma/rdma_memory_region.h/.cpp` with no generic pool/cache/reuse layer; add only the Pull descriptor/control messages needed to `proto/rdma_control.proto` and regenerate through existing CMake rules.
- [ ] T010 [US2] Add `modules/store/node/storage_rdma_service.h/.cpp`, return from the Pull service method after posting READ, retain the server RPC/region context through READ completion and store use, then run the response closure after `ChunkStore::WriteChunk()` completes; keep identity, checksum, conflict, and durability decisions in the store layer.

**Stage gate**: T008 passes; no success is emitted at RDMA READ completion alone.

## Stage 3 — User Story 3: Push V1 (P3)

**Goal**: A fixed SlotPool per Client connection supports bounded RDMA WRITE with generation safety and no global slot competition.

- [ ] T011 [US3] Extend `tests/rdma_transport_contract_test.cpp` with Push cases for immediate return after posting WRITE, WRITE-CQE callback posting READY on the same RC QP, per-client pool isolation, `FREE → WRITING → READY → FLUSHING → FREE`, stale pool/slot generation, ready-before-write rejection, durable ACK, safe reuse, and pre-transfer capacity/resource rejection.
- [ ] T012 [US3] Implement authoritative connection-scoped SlotPools plus the minimal connection-scoped `RemoteSlotMap` mirror in `modules/rdma/rdma_slot_pool.h/.cpp`; add pool initialization/fetch and READY/COMMIT messages to `proto/rdma_control.proto`, with no global pool, cross-client CAS, or per-chunk `AcquireSlot`/`ReleaseSlot` path.
- [ ] T013 [US3] Implement asynchronous Push handling in `modules/store/node/storage_rdma_service.cpp`, including WRITE-completion callback→same-RC-QP READY, retained server response context, and delegation to `ChunkStore::WriteChunk()` before durable ACK; wire `storage_node_app` directly to `rdma_core` for RDMA server lifecycle without changing gRPC service behavior.

**Stage gate**: T011 passes; clients cannot see/use another Client’s pool, stale writes cannot publish, and every reusable slot is demonstrably safe.

## Stage 4 — User Story 4: Preferred RDMA + Safe Fallback (P4)

**Goal**: RDMA becomes the preferred write transport while gRPC remains a strictly safe fallback.

- [ ] T014 [US4] Add the five-state fallback matrix to the focused RDMA and storage-transfer tests, including temporary pool exhaustion, unsuitable assigned slot capacity, remote-buffer/RDMA-resource shortage rejected before transfer, illegal system-size/metadata/protocol input, and failures immediately after posting the remote data operation.
- [ ] T015 [US4] Add a minimal preferred-transport decorator/factory in `modules/store/transfer/storage_transfer_client.h/.cpp` that maps outcomes to `not_started`, `rejected_before_transfer`, `remote_state_uncertain`, `failed`, or `durable_success` and falls back only for the first two.
- [ ] T016 [US4] Wire RDMA mode and host-plus-configured-port-offset derivation in the storage client/application configuration, link the existing `storage_client` target directly to `rdma_core`, and preserve existing View, Metadata, Raft, read, and gRPC contracts without adding an RDMA dependency to the Raft core.
- [ ] T017 [US4] Verify object upload still records only durable replica results and calls the unchanged `CommitObject` boundary; add the smallest regression assertion to the existing object/storage transfer tests rather than duplicating orchestration.

**Stage gate**: All fallback matrix cases and existing gRPC storage-transfer tests pass.

## Stage 5 — User Story 5: Client RemoteSlotMap (P5)

**Goal**: Reuse only the current Client’s assigned Push descriptors without weakening Store authority or generation validation.

- [ ] T018 [US5] Add map contract cases to `tests/rdma_transport_contract_test.cpp` proving it contains only the current Client’s pool, selects local `FREE` slots without per-chunk ownership RPCs, reuses same-generation entries, and invalidates on restart, reconnect, connection/pool generation change, slot/rkey-generation change, and explicit invalidation.
- [ ] T019 [US5] Harden the Stage 3 mirror into the bounded per-client `StorageNodeId -> RemoteSlot[]` map in `modules/rdma/rdma_slot_pool.h/.cpp`; refresh the assigned pool after invalidation while keeping Store pool lifecycle/generation authoritative.

**Stage gate**: T018 passes and cached descriptors can never outlive their validated generations.

## Final Validation and Documentation

- [ ] T020 Update user-facing build/configuration notes and `modules/rdma/module-notes.md` with asynchronous callback/lifetime rules, on-demand `RdmaMemoryRegion`, fixed RECV buffers and Push SlotPool exceptions, actual option names, endpoint-offset limitations, and the boundary between RDMA completion and durable completion.
- [ ] T021 Run `CTEST_PARALLEL_LEVEL=1 ./test.sh --group unit` and `CTEST_PARALLEL_LEVEL=1 ./test.sh --group persistence`, store logs under `tmp/test-logs/`, and report only PASS/timing or the prescribed failure summary.

## Dependencies

```text
Stage 0 -> Stage 1 -> Stage 2 -> Stage 3 -> Stage 4 -> Stage 5 -> Final
```

- Pull and Push depend on the Stage 1 control chain.
- Safe fallback depends on at least one durable RDMA write mode; implement it after both modes to validate one common policy.
- `RemoteSlotMap` depends on authoritative per-client SlotPool allocation and generation behavior.
- Tests within each stage precede the implementation tasks they constrain.

## Scope Control

The task list intentionally excludes service registries, operation-specific control/READ/WRITE/READY QPs, global cross-client SlotPools/CAS, per-chunk slot ownership RPCs, shared CQ runtime, request manager, task scheduler, priority queues, timeout wheels, automatic reconnect framework, generic `RdmaMemoryPool`, selective signaling, SPDK/FastBlock, new general runtimes, read-path RDMA, discovery-contract changes, benchmarks, and stress infrastructure.
