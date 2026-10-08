# Contract: Transport Selection

## Client Construction

- `CreateGrpcStorageTransferClient()` 保持现有纯 gRPC 行为。
- 新 preferred factory 组合一个 RDMA client 和现有 gRPC fallback；不修改调用方的 `WriteChunk` 输入和 durable result 语义。
- RDMA preference 默认开启，但只有 capability available 时才发起连接。

## Endpoint Resolution

- 输入继续使用现有 StorageNode `host:port` endpoint。
- RDMA endpoint 使用同 host 加共享、可覆盖的 port offset 推导。
- 非法 host/port、加 offset 溢出或平台不支持时返回 not_started。
- 第一版不修改 ViewNode/proto 来传播独立 RDMA endpoint。

## Selection Algorithm

```text
if RDMA preference is off or capability unavailable:
    call existing gRPC client
else:
    attempt configured RDMA mode
    if durable_success or explicit non-retryable failure:
        return its mapped result
    if not_started or rejected_before_transfer:
        call existing gRPC client with unchanged identity
    if remote_state_uncertain:
        return explicit uncertain failure; do not fallback
```

## Compatibility

- ReadChunk 继续使用现有 gRPC path；第一版只优化 upload/write。
- 没有 RDMA 硬件、依赖或 listener 时，外部上传行为与当前 gRPC 基线一致。
- non-Linux implementation 只能返回 unavailable，不得返回伪 completion 或 durable success。
