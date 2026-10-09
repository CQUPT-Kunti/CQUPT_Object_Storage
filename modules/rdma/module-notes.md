# RDMA 模块说明

## 模块职责

`modules/rdma/` 是 011 阶段的 RDMA Fast Path 通用传输模块，负责：

- 基于 `librdmacm` + `libibverbs` 的 Client / Server 连接生命周期
- 实现 `google::protobuf::RpcChannel` 的异步控制通道
- 同一 RC QP 上的 SEND/RECV、RDMA READ、RDMA WRITE、READY/COMMIT
- 按需注册的临时 `RdmaMemoryRegion` 与 per-client `SlotPool`

本模块不负责 chunk 持久化、identity/checksum/conflict 判定、metadata/Raft/View；这些仍由现有组件与 Store 适配层负责。

## 文件布局（与 plan 一致）

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

## 核心类及职责

```text
RdmaConnection
    Client 连接、异步 RpcChannel

RdmaServer
    Server 监听和接受连接

RdmaServerConnection
    服务端连接和异步 completion

RdmaMemoryRegion
    临时 MR 注册与释放

RdmaSlotPool
    Store 为每个 Client 分配的 SlotPool

RemoteSlotMap
    Client 缓存属于自己的远端 Slot
```

- `RdmaConnection`：Client 侧连接与异步 `RpcChannel`。`CallMethod()` 分配 `request_id`、序列化请求、登记 `PendingRpc`、投递 SEND 后立即返回；CQ completion 按 `request_id` 匹配响应、解析、更新 controller、移除 pending，并执行 `done` 恰好一次。
- `RdmaServer`：Server 监听/accept 边界，只持有恰好一个 `google::protobuf::Service*`；按 descriptor 查找方法，不引入 service registry、插件机制或通用异步 dispatcher。
- `RdmaServerConnection`：服务端每个 accepted connection 的连接上下文（随 `rdma_server.h/.cpp` 落地），复用与 Client 相同的单 CQ completion-thread 模式；负责 RECV、请求解析、`ServerRpcContext` 与响应 SEND 的资源生命周期。`Service::CallMethod()` 返回不代表 RPC 完成，只有 service closure 被调用后才序列化并投递响应。
- `RdmaMemoryRegion`：按需 `malloc → ibv_reg_mr → post WR → ibv_dereg_mr → free` 的临时 MR 包装；无通用 pool/cache/reuse。
- `RdmaSlotPool`：Store 为单个 Client connection 创建、注册并独占的固定有界 slot 池；slot 状态为 `FREE → WRITING → READY → FLUSHING → FREE`，带 pool/slot generation。
- `RemoteSlotMap`：Client 侧只镜像自己从 Store 获得的 slot 描述（`slot_id`、`remote_addr`、`rkey`、`capacity`、`generation`、`state` 及 pool/connection generation）；不发起 per-chunk 所有权请求，Store 始终是权威。
- `rdma_transport.h/.cpp/.linux`：平台边界。Linux 提供 CM/verbs 实现；其他平台提供 portable unavailable backend。

## 异步 RPC 流程

```text
Protobuf Stub
    ↓
RdmaConnection::CallMethod()
    ↓
Serialize + post SEND
    ↓
立即返回
    ↓
CQ completion thread
    ↓
匹配 request_id
    ↓
Parse Response
    ↓
done->Run()
```

- 每个 WR 使用 lifetime-safe `WrContext { type, request_id, slot_id }`，通过 `wr_id` 区分 SEND/RECV/READ/WRITE，不做 bit packing。
- 请求 SEND buffer 存活到 SEND completion；调用方保证 `controller`、`response`、`done` 存活到异步完成。
- 连接关闭时停止并 join completion thread，确定性地失败/完成剩余 pending RPC 并释放资源。

## Pull 模式

```text
Client 注册 MR
    ↓
发送 addr/rkey
    ↓
Store RDMA READ
    ↓
ChunkStore::WriteChunk()
    ↓
durable ACK
```

- Client 按需注册源 MR，发控制请求（identity、checksum、length、addr/rkey、generation）。
- Server 按需注册目标 MR，投递 RDMA READ 后从业务方法返回；READ completion 后才校验并交给 Store 适配层。
- 响应 closure 只在 durable 结果产生后投递；Client 收到最终响应后才可释放源 MR。

## Push 模式

```text
Store 为 Client 分配 SlotPool
    ↓
Client 获取自己的 RemoteSlotMap
    ↓
RDMA WRITE
    ↓
WRITE completion
    ↓
同一 RC QP 发送 READY
    ↓
Store WriteChunk
    ↓
durable ACK
```

- Store 在连接初始化/建池时把属于该连接的 slot descriptors 返回给 Client。
- Client 只从自己的 `RemoteSlotMap` 选择本地 `FREE` slot 并标记 `WRITING`，投递 RDMA WRITE 后返回。
- WRITE completion 回调在同一 RC QP 上投递 READY/COMMIT；Store 校验 connection/pool/slot generation，经 `ChunkStore::WriteChunk()` 持久化后才 ACK，并仅在可安全复用时把 slot 还原为 `FREE`。
- WRITE completion 与 READY 都不是 durable 成功。

## 内存生命周期

按需分配/注册、用完释放：

- 控制请求/响应等普通 scratch buffer 使用 `RdmaMemoryRegion`，从对应 SEND completion 或最终响应清理路径释放。
- Pull 的 Client 源 MR 存活到最终响应证明 server READ 完成；Server 目标 MR 存活到 READ completion 且 checksum/store 消费结束。
- 每个 `WrContext` 存活到其 CQE 被处理。

必须在有效期内保持注册：

- 每个连接固定的一组 RECV buffer：SEND 要求预投递接收；处理完成后重投递，连接停止且确认无未完成 RECV WR 后才释放。
- Push SlotPool 的 MR：在对外发布的 `addr/rkey` descriptor 有效期间保持注册，属于协议状态而非通用内存池。

## 与其他模块的边界

- Storage 适配（descriptor → 写请求、调用 `ChunkStore::WriteChunk()`）在 `modules/store/node/storage_rdma_service.*`，不在本模块。
- `storage_node_app` 直接链接 `rdma_core` 承载 server/adapter 路径；`storage_client` 链接 `rdma_core` 承载 client 路径；Raft core 不引入 RDMA 依赖。
- endpoint 规则：MVP 由现有 host + 配置端口偏移推导，不改动 View、Metadata、Raft 契约。
