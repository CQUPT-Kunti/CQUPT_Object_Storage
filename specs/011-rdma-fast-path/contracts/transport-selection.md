# Contract: Transport Selection

## Scope

Transport selection applies only to storage-node chunk writes. Reads and all View, Metadata, Raft, manifest, and `CommitObject` calls retain their existing paths.

## Configuration Modes

| Mode | Startup/build behavior | Write behavior |
|---|---|---|
| `OFF` (default) | Do not require `rdmacm`/`ibverbs`; expose the explicit unavailable RDMA backend | Use gRPC |
| `ON` | Build the RDMA module and link `rdmacm`/`ibverbs` directly; missing libraries fail configure/compile/link | Prefer RDMA, otherwise choose gRPC only through the attempt-state rules |

Pull is the initial/default RDMA data mode. Push becomes selectable after Stage 3.

## Per-Write Selection

```text
StorageTransferClient::WriteChunk
              │
              ▼
      RDMA enabled for target?
        │ no              │ yes
        ▼                 ▼
      gRPC        begin tracked RDMA attempt
                           │
                           ▼
                 terminal attempt state
                           │
          ┌────────────────┼──────────────────────┐
          ▼                ▼                      ▼
 not_started /      remote_state_uncertain   failed / durable_success
 rejected_before_          │                      │
 transfer                  ▼                      ▼
          │             reconcile            return result
          ▼
        gRPC
```

Selection is not based only on an exception or status string. It uses the explicit attempt state defined by the durability contract.

`CallMethod()` returning means only that asynchronous submission succeeded; it does not establish transfer or durable success. The attempt-state boundary follows actual WR posting and CQ/response evidence. Once a remote data WR may have executed, callback failure cannot be converted into automatic gRPC fallback.

## Capacity and Resource Classification

The following map to `rejected_before_transfer` only when explicitly detected before posting RDMA READ/WRITE: the current Client’s SlotPool has no `FREE` slot, available assigned slots have insufficient capacity for an otherwise-valid chunk, an on-demand `RdmaMemoryRegion` cannot be allocated/registered, remote buffers are temporarily exhausted, or other RDMA resources are temporarily unavailable. gRPC fallback is safe because no remote data operation began.

A chunk above the system-wide maximum, invalid chunk metadata, identity conflict, invalid checksum fields, or invalid protocol fields maps to `failed`. These errors are not transport availability problems and MUST NOT be hidden by fallback.

## Endpoint Resolution

V1 derives the RDMA endpoint from the host already returned by View plus a configured port offset. This is an MVP deployment rule only:

- it does not modify the current View record;
- it does not infer RDMA durability support from endpoint presence;
- connection/probe failure before transfer maps to a safe pre-transfer state;
- the offset and collision policy are configuration/tuning decisions.

A future discovery revision may advertise `grpc_endpoint`, `rdma_endpoint`, and `rdma_supported`. That change is outside this feature’s V1 contract.

## Push Descriptor Cache (Stage 5)

For each `StorageNodeId`, the Client mirrors only the SlotPool that the Store assigned to that Client connection. Normal writes select a local `FREE` slot and do not request ownership for every chunk. Eligibility requires matching connection, pool, and slot generations. Restart, reconnect, generation/rkey change, or explicit invalidation discards entries and forces a pool refresh. The mirror never overrides Store authority or exposes another Client’s slots.

## Existing Orchestration

- The object-transfer layer continues to control bounded fanout and byte budgeting.
- Only `durable_success` contributes to successful replica accounting.
- Failed/uncertain writes follow existing cleanup/reconciliation policy.
- Metadata `CommitObject` remains unchanged and occurs only after the required durable replica results.
