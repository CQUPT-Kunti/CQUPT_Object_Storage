# Quickstart: Stage-by-Stage RDMA Validation

This guide is for implementation and review. Complete one stage before starting the next. Commands assume the repository root and the existing low-parallel preset, whose binary directory is `build/linux`.

## Stage 0 — Build Boundary

Configure with RDMA disabled (default):

```bash
cmake --preset debug-ninja-low-parallel -DCQUPT_RDMA=OFF
cmake --build --preset debug-ninja-low-parallel --target rdma_core rdma_transport_contract_test
```

Then configure with RDMA enabled:

```bash
cmake --preset debug-ninja-low-parallel -DCQUPT_RDMA=ON
cmake --build --preset debug-ninja-low-parallel --target rdma_core rdma_transport_contract_test
```

Expected: `OFF` uses the explicit unavailable backend and does not require RDMA libraries. `ON` builds the Linux backend and links `ibverbs` and `rdmacm` directly; when either library is missing, configure, compile, or link fails directly, with no extra detection or fallback.

## Stage 1 — Minimal Async RpcChannel

Build and run the focused contract test:

```bash
cmake --build --preset debug-ninja-low-parallel --target rdma_transport_contract_test
ctest --test-dir build/linux -R '^rdma_transport_contract$' --output-on-failure
```

Evidence required: unavailable error, `librdmacm` client/server lifecycle, exactly one RC QP, `CallMethod()` returning before response completion, atomic request ids, pending-map correlation, per-connection CQ thread, SEND/RECV `WrContext`, asynchronous server closure, response parsing, callback-once behavior, fixed RECV re-posting, and deterministic cleanup.

## Stage 2 — Pull

Run the same focused target after adding Pull cases:

```bash
cmake --build --preset debug-ninja-low-parallel --target rdma_transport_contract_test
ctest --test-dir build/linux -R '^rdma_transport_contract$' --output-on-failure
```

Evidence required: large bytes move through asynchronous RDMA READ rather than Protobuf; client and server temporary `RdmaMemoryRegion` objects are registered on demand and callback-released after their final use; bad checksum is rejected; success follows the durable store boundary; no generic MemoryPool exists.

## Stage 3 — Push V1

```bash
ctest --test-dir build/linux -R '^rdma_transport_contract$' --output-on-failure
```

Evidence required: one isolated SlotPool per Client connection, no per-chunk ownership RPC, `FREE → WRITING → READY → FLUSHING → FREE`, business-thread return after WRITE posting, WRITE-completion callback posting same-RC-QP READY, stale generation rejection, durable ACK, and safe reuse.

## Stage 4 — Preferred RDMA and Fallback

```bash
ctest --test-dir build/linux -R '^(rdma_transport_contract|storage_transfer_client)$' --output-on-failure
```

Evidence required: gRPC fallback occurs only for `not_started` and `rejected_before_transfer`; uncertain, definitive-failure, and durable-success states never fall back.

## Stage 5 — RemoteSlotMap

```bash
ctest --test-dir build/linux -R '^(rdma_transport_contract|storage_transfer_client)$' --output-on-failure
```

Evidence required: the map contains only the current Client’s assigned pool, reuses local slots within one generation, and invalidates on restart, reconnect, connection/pool-generation change, rkey/slot-generation change, and explicit invalidation.

## Optional Hardware Smoke Test

After the contract tests pass, run a single-node write on supported RDMA hardware with `CQUPT_RDMA=ON`. Confirm that `CallMethod()` returns before the response, the ordinary CQ thread drives callbacks, chunk bytes use asynchronous Pull/Push, temporary regions are released after completion, and the client records success only after the durable ACK. This is smoke evidence, not a replacement for the deterministic contract tests.

## Final Regression

Save logs locally and keep parallelism low:

```bash
mkdir -p tmp/test-logs
CTEST_PARALLEL_LEVEL=1 ./test.sh --group unit >tmp/test-logs/rdma-unit.log 2>&1
CTEST_PARALLEL_LEVEL=1 ./test.sh --group persistence >tmp/test-logs/rdma-persistence.log 2>&1
```

Do not advance a stage by skipping a failed test. Benchmarks, stress tests, and a multi-node performance gate are outside this feature.
