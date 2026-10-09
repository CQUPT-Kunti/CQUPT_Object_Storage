# Contract: Durability and Fallback

## Inherited Authority

RDMA changes how bytes reach a storage node; it does not change what makes them durable or visible.

- `ChunkStore::WriteChunk()` remains the chunk validation, conflict, idempotency, staging, flush, publish, and directory-sync authority.
- A replica result is successful only when the existing store result says it is durable.
- Metadata `CommitObject` remains the object visibility boundary after the required durable replica results exist.
- Manifest handling, cleanup candidates, reads, Metadata, Raft, and View behavior remain unchanged.

## Completion Levels

```text
control accepted
      ↓
remote RDMA operation posted
      ↓
RDMA completion        (bytes moved; not durable)
      ↓
identity/size/checksum validated
      ↓
ChunkStore::WriteChunk
      ↓
durable ACK            (replica may be counted)
      ↓
CommitObject           (object becomes visible)
```

No earlier event may be reported as durable success.

All RDMA steps are asynchronous. Returning from `CallMethod()` or a Store service method reports only successful submission/local dispatch. CQ callbacks advance the operation, and the caller’s final `done` closure runs only after the response corresponding to the durable result is parsed (or a terminal error is established).

## Attempt-State Contract

| State | Required evidence | Retry/fallback behavior |
|---|---|---|
| `not_started` | No RDMA READ/WRITE was posted and server did not begin the write | gRPC fallback allowed |
| `rejected_before_transfer` | Explicit response proves rejection before data movement, including temporary per-client SlotPool exhaustion, no suitable assigned slot capacity, remote-buffer shortage, or RDMA resource shortage | gRPC fallback allowed |
| `remote_state_uncertain` | Timeout/disconnect/error occurred after posting or remote execution cannot be disproved | No blind fallback; reconcile with identical chunk/request identity |
| `failed` | Explicit invalid metadata/protocol, chunk above the system maximum, invalid checksum fields, identity conflict, or other definitive non-retryable result | Return failure; no fallback |
| `durable_success` | Store returned durable success, including a compatible already-existing chunk | Record success; no fallback |

Implementation exceptions are never sufficient evidence for `not_started`; the state must follow tracked transport progress.

## Pull Lifetime Contract

- Client and server Pull buffers use on-demand `RdmaMemoryRegion` objects rather than a generic pool.
- The client owns and keeps the source region registered until the final response confirms RDMA READ completion or connection teardown establishes that reuse is safe.
- The server keeps its destination region registered through READ completion and allocated through checksum/store consumption before callback-driven deregistration/free.
- Checksum or identity failure is `failed`, not a reason to retry the same bytes through gRPC.
- The server service method may return immediately after posting READ; its retained completion closure sends durable success only after the READ callback and `ChunkStore::WriteChunk()` succeed.

## Push Slot Contract

- The Store creates and registers one fixed, bounded SlotPool for each Client connection. Pools are not shared across Clients.
- The Client receives only its own pool descriptors and mirrors them in `RemoteSlotMap`; normal writes select a local `FREE` slot without requesting per-chunk ownership.
- The Client posts RDMA WRITE and returns; its WRITE-completion callback posts READY/COMMIT SEND on the same `RdmaConnection` and RC QP.
- Ready/commit is accepted only for the current connection/pool/slot generation and a matching write.
- The slot is not reusable while validation or durable publication is in progress.
- Success/failure cleanup returns the slot to `FREE` only when prior DMA and Store use can no longer access it.
- The Store remains authoritative for pool creation/destruction, MR registration, generation, disconnect/reconnect, and restart invalidation.

Push SlotPool MRs remain registered while their exported descriptors are valid. This protocol lifetime and the small fixed per-connection RECV buffers are not a general MemoryPool or registered-buffer reuse layer.

## Failure Examples

| Event | State |
|---|---|
| RDMA disabled before connect | `not_started` |
| Current Client pool has no `FREE` slot and Store rejects before WRITE | `rejected_before_transfer` |
| Assigned slots are too small for this otherwise-valid chunk and Store rejects before WRITE | `rejected_before_transfer` |
| Remote buffer or RDMA resource is temporarily unavailable before READ/WRITE | `rejected_before_transfer` |
| Connection drops after RDMA WRITE is posted | `remote_state_uncertain` |
| Chunk exceeds the system-wide maximum or has invalid protocol metadata | `failed` |
| Server reports checksum mismatch | `failed` |
| Server reports same identity and compatible durable existing content | `durable_success` |

## Platform Contract

`CQUPT_RDMA=REQUIRED` cannot silently fall back to an unavailable/no-op RDMA backend. On Linux, configuration or startup must return a clear error if either `librdmacm` or `libibverbs` is unavailable. `AUTO` may select the existing gRPC path before any RDMA attempt begins.
