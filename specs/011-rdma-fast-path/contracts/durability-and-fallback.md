# Contract: Durability And Fallback

## Durable Success

- RDMA READ/WRITE completion 只证明网络操作完成。
- durable success 必须来自现有 `ChunkStore::WriteChunk()` 成功结果。
- `already_exists=true` 只有在现有 same-identity/same-checksum 幂等规则成立时可作为 durable success。
- 未获得 durable success 的节点不得进入 committed manifest。

## Fallback Eligibility

| Failure point | Classification | gRPC fallback |
|---|---|---|
| RDMA compiled/runtime unavailable | not_started | Yes |
| endpoint derivation or connect fails | not_started | Yes |
| slot/descriptor rejected before data op | rejected_before_transfer | Yes |
| Pull request accepted then connection lost | remote_state_uncertain | No |
| Push WRITE posted then connection lost | remote_state_uncertain | No |
| Network completion received, durable ACK lost | remote_state_uncertain | No |
| Explicit checksum/invalid/conflict response | explicit non-retryable failure | No |
| ChunkStore durable success | durable_success | No |

## Identity

- fallback 复用原始 `request_id`、`ChunkIdentity`、offset、expected size 和 checksum。
- 不生成“fallback 专用”的新 chunk id 或 object version。
- 同一 attempt 的 diagnostics 必须能显示 RDMA 阶段和是否尝试 gRPC。

## Object Visibility

- RDMA client/server 不调用 Metadata `CommitObject`。
- 现有 upload orchestration 只收集 durable result，并继续决定 quorum、cleanup candidate 与 CommitObject。
- chunk durable 但 CommitObject 失败时，对象保持不可见，继续使用现有 orphan/cleanup 语义。

## Crash And Restart

- StorageNode crash 前仅在 RAM 的 slot 不可恢复，也不得在 restart 后报告 durable。
- 已通过 `ChunkStore` publish 的 chunk 按现有 restart rebuild 语义恢复。
- restart 后 generation 变化，所有旧 MR/slot facts 明确失效。
