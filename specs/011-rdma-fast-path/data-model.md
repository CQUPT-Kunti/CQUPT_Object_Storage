# Data Model: RDMA Fast Path

## 1. RDMA Connection

**Purpose**: 表达一个客户端到 StorageNode 的可关闭连接及其资源 generation。

**Fields**:

- endpoint：由现有 StorageNode endpoint 和 RDMA port offset 推导。
- generation：本次 listener/connection 代次；重启或重建后变化。
- capability：available / unavailable，并携带明确原因。
- state：disconnected → connecting → connected → closing → closed。
- outstanding operation：第一版最多一个同步操作。

**Rules**:

- closed connection 的 QP、CQ、MR 必须按反向构造顺序释放。
- generation 不匹配的 descriptor/lease 一律拒绝。
- connection failure 只有发生在数据操作前才可标记 safe fallback。

## 2. Remote Buffer Descriptor

**Purpose**: Pull 模式中让 StorageNode 读取客户端 bounded chunk buffer。

**Fields**:

- address、rkey、length。
- connection generation。
- request id、chunk identity 的绑定摘要。
- access：只允许 remote read。

**Validation**:

- length 必须等于 expected size，且不超过单 chunk 上限。
- descriptor 只在所属连接和 operation 生命周期内有效。
- client 收到服务端 read-complete/durable result 前不得 deregister/reuse。

## 3. Slot Lease

**Purpose**: Push 模式中表达 StorageNode 权威授予的一次写入权。

**Fields**:

- slot id、address、rkey、capacity。
- owner connection、request id、chunk identity。
- generation。
- state：FREE / RESERVED / WRITING / READY / FLUSHING / FREE。

**Transitions**:

```text
FREE --reserve--> RESERVED --client post--> WRITING
WRITING --ready--> READY --validate--> FLUSHING
FLUSHING --durable success/failure cleanup--> FREE
RESERVED/WRITING --disconnect or generation change--> invalidated -> FREE
```

**Rules**:

- 同一时刻一个 slot 只有一个 owner。
- 第一版 lease 不缓存；完成或失败后必须重新申请。
- `READY` 之前不能调用 `ChunkStore::WriteChunk()`；`FLUSHING` 完成之前不能复用 slot。

## 4. Chunk Transfer Attempt

**Purpose**: 关联一次 RDMA 尝试与可能的 gRPC fallback。

**Fields**:

- request id、chunk identity、target node。
- mode：pull / push。
- attempt state：not_started / rejected_before_transfer / remote_state_uncertain / durable_success。
- transport diagnostic：capability、endpoint、operation stage、system error。
- storage result：status、durable、already_exists、checksum、metadata。

**Rules**:

- fallback 必须沿用相同 request id 与 chunk identity。
- uncertain 不得转换为 retryable fallback。
- durable success 必须来自现有 `ChunkStore` 响应，不来自 CQ completion。

## 5. Preferred Transfer Decision

**Purpose**: 在 RDMA 与现有 gRPC client 之间做唯一、可测试的选择。

**Inputs**:

- runtime preference、compiled capability、derived endpoint。
- RDMA attempt state。
- 原始 `StorageTransferWriteRequest`。

**Outputs**:

- selected transport。
- whether fallback attempted。
- 最终现有 `StorageTransferWriteResult`。
- RDMA/fallback diagnostics。

**Invariant**: 只有 `not_started` 和 `rejected_before_transfer` 能进入 fallback；其它状态直接返回。

## Persisted Data Impact

- 无新 persisted entity。
- slot、MR、connection、attempt 均为进程内短生命周期状态。
- chunk 文件、ChunkIndex、metadata manifest、Raft log/snapshot 格式不变。
