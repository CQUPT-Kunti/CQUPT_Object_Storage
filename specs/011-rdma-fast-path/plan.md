# Implementation Plan: RDMA Fast Path

**Branch**: `011-rdma-fast-path` | **Date**: 2026-10-08 | **Spec**: [spec.md](spec.md)  
**Input**: Feature specification from `/specs/011-rdma-fast-path/spec.md`

## Summary

在现有 `StorageTransferClient -> StorageNode -> ChunkStore` 链路旁增加一条可选的 Linux RDMA Fast Path。RDMA 可用时优先执行单 chunk 数据搬运；只有在远端数据操作明确尚未开始时才回退现有 gRPC 写入。服务端仍调用现有 `ChunkStore::WriteChunk()` 并等待 durable publish，客户端仍由现有 upload orchestration 汇总 durable facts、调用 `CommitObject`。

按最小可交付顺序完成四个阶段：最小连接/探测、Pull、Push、上传集成与安全 fallback。第一版采用 `librdmacm + libibverbs`、同步调用加单一 completion 处理边界；不做通用 RPC runtime、客户端 slot cache、复杂异步状态机或协议重写。

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: 现有 gRPC、Protobuf、GoogleTest、CMake、C++ standard library；Linux 可选 `librdmacm` 与 `libibverbs`  
**Storage**: 继续由 `LocalDiskChunkStore` 完成 staging → flush → publish → directory sync → LIVE index update；RDMA 不新增持久化格式  
**Testing**: GoogleTest + CTest；每阶段一个 targeted test 命令，最终一次相关测试集合  
**Target Platform**: Linux 提供真实 RDMA；Windows/macOS 和缺少 RDMA 能力的 Linux 构建提供明确 unsupported 结果并走现有路径  
**Project Type**: Raft metadata control-plane + StorageNode chunk data-plane  
**Performance Goals**: 第一版完成单连接、单 chunk（至少 64 MiB）RDMA 搬运；内存上界由一个 pull buffer 或固定数量/容量的 push slots 决定  
**Constraints**: 不改变既有 proto 语义、持久化格式、CommitObject 可见性、manifest 内容规则和现有 `CreateGrpcStorageTransferClient()` 行为；不允许网络 completion 冒充 durable success  
**Scale/Scope**: 单客户端、单连接、单 chunk、固定小 slot pool；整对象多 chunk RDMA 并发、长生命周期 slot cache 和性能调优延期

## Constitution Check

*GATE: Phase 0 前检查，Phase 1 设计后复查。*

- **Preserve The Verified Core — PASS**: 保留现有 gRPC transfer、chunk identity、幂等写、durable publish、manifest-driven read 与 CommitObject；RDMA 是可选旁路。
- **Protocol/public API/persisted format — PASS**: 不修改现有 `.proto` 与落盘格式。仅新增内部 RDMA wire v1 和 additive C++ factory/config；原 gRPC factory 行为不变。
- **Durability/recovery — PASS**: 服务端 RDMA 收到 payload 后仍进入同一个 `ChunkStore::WriteChunk()`；StorageNode 重启使 connection generation、MR 和 slot 全部失效，不恢复未发布 RAM 内容。
- **Cross-platform — PASS**: verbs 代码隔离在 Linux `.cpp`；其他平台返回明确 unsupported，现有 gRPC fallback 保持可用。显式要求 RDMA 而依赖缺失时 configure 失败，不静默伪装成功。
- **Test entry points — PASS**: 使用一个 RDMA contract target、一个 transfer integration target，以及最终相关 CTest 过滤集合；不新增大规模组合矩阵。
- **Observability/minimal surface — PASS**: 只增加 transport、generation、attempt state、fallback eligibility 与 durable result 诊断；不复制 metadata 或 Raft 状态。

## Current Baseline From Targeted Inspection

- `modules/store/transfer/object_transfer.cpp` 已提供 bounded chunk upload、replica fan-out、in-flight bytes 控制、actual durable replica manifest 和最终 `CommitObject` gate。
- `StorageTransferClient::WriteChunk()` 是最窄的单 StorageNode 写入替换点；当前 `GrpcStorageTransferClient` 复用同一 request/chunk identity 做有限重试。
- `StorageNodeService::WriteChunk()` 已完成 RPC 到 `ChunkStore::WriteChunk()` 的适配，成功只表示 chunk durable，不表示对象已 committed。
- `LocalDiskChunkStore::WriteChunk()` 已保证同内容幂等、不同内容 conflict，并实现真实 durable publish；RDMA 不应复制这段逻辑。
- StorageNode discovery 当前只暴露现有 data-plane endpoint。第一版不改 View/proto，而是从现有 host:port 按一个共享、可覆盖的 RDMA port offset 推导 RDMA endpoint；推导失败即明确 unavailable。
- `google::protobuf::RpcChannel` 仍存在，但官方将这套 proto2 generic service API 标为 deprecated；当前仓库生成的是 gRPC stub。第一版不为学习目标引入第二套通用 RPC runtime。

## Design Decisions

### 1. Build And Capability Boundary

- 新增 `CQUPT_RDMA_MODE=AUTO|ON|OFF` cache string，默认 `AUTO`。
- `AUTO`：Linux 找到 `librdmacm`/`libibverbs` 时编译真实实现，否则编译 portable unavailable 实现并输出清晰 configure status。
- `ON`：非 Linux或缺少依赖时 configure 明确失败；不得回落成 no-op success。
- `OFF`：只编译 unavailable 实现，运行时由 preferred client 直接走现有 gRPC。
- 真实实现只放在 `modules/store/rdma/rdma_transport_linux.cpp`；共享 `.h/.cpp` 不包含 verbs 头。

### 2. Narrow Transport Surface

- 新增一个最小 `RdmaClient`/`RdmaServer` 边界和 factory；Linux 与 unavailable 实现共享相同结果分类。
- control header 只包含 magic、version、operation、request id length、serialized metadata length、descriptor/slot facts 和 connection generation。
- chunk metadata 复用现有写请求/响应的字段语义，payload 留空并由 RDMA 数据面搬运；不新增或修改现有 `.proto`。
- 不实现 pending map、priority queue、batching、shared CQ、selective signaling 或通用 method registry。
- 不把 `google::protobuf::RpcChannel` 放进生产上传路径。若后续确有教学需求，作为独立实验特性另行规划。

### 3. Pull Before Push

- **Pull**：客户端注册当前 bounded chunk buffer，发送只读 descriptor；服务端 RDMA READ 到一个 bounded buffer，校验并调用现有 `ChunkStore::WriteChunk()`，durable response 返回前客户端不得释放 MR。
- **Push**：服务端启动时创建固定数量、固定容量 slots；客户端每次通过控制消息申请一个 lease，RDMA WRITE 后发送 ready；服务端校验、写盘并释放。
- 第一版不缓存 remote slots。每次 chunk 重新申请，StorageNode 始终是 ownership 权威；只有控制面往返被证明是瓶颈后才引入 cache。
- 连接断开、服务重启或 generation 变化立即使 MR descriptor/slot lease 失效。

### 4. Durable Result And Fallback Matrix

RDMA attempt 只保留四类内部结果：

| Attempt state | Remote data may exist | Fallback | Upload result |
|---|---:|---:|---|
| `not_started` | No | Yes | 走现有 gRPC |
| `rejected_before_transfer` | No | Yes | 释放资源后走现有 gRPC |
| `remote_state_uncertain` | Maybe | No | 显式 timeout/IO + uncertain 诊断 |
| `durable_success` | Yes, durable | No | 返回现有 durable success facts |

- Pull descriptor 已被服务端接受，或 Push RDMA WRITE 已 post 后失联，均归类为 uncertain。
- checksum mismatch、invalid request 等收到明确服务端结果时直接返回对应非 retryable 失败，不通过 fallback 掩盖错误。
- preferred wrapper 复用同一 `request_id` 和 `ChunkIdentity` 调用 gRPC fallback。
- 最终对象可见性和 manifest 仍完全由现有 upload/CommitObject 流程决定。

### 5. Endpoint And Runtime Limits

- 第一版从 discovery 的 StorageNode endpoint 推导同 host 的 RDMA port，默认 offset 为一个共享常量；client/server 配置允许覆盖 offset，避免把硬件/部署差异写死。
- 端口溢出、地址解析失败、listener 未启动都视为 `not_started`，可安全 fallback。
- Pull 同时只保留一个 chunk buffer；Push 使用固定 `slot_count` 与 `slot_capacity`，不动态扩容。
- completion 采用 blocking/single-handler 模型；只有出现可复现吞吐瓶颈后才考虑更复杂 CQ 架构。

## Delivery Stages

### Stage 1 — Connection And Probe

- 建立 optional build、portable unavailable 实现和 Linux connection/probe/cleanup。
- 完成最小 SEND/RECV 请求响应与 generation 校验。
- 阶段验证：一个 `rdma_transport_contract` targeted command；无 RDMA 设备时验证明确 unavailable，有设备时执行真实 probe。

### Stage 2 — Pull Data Path

- 客户端注册单 chunk buffer；服务端 READ、checksum、调用既有 `ChunkStore::WriteChunk()` 并返回 durable result。
- 阶段验证：同一 contract target 增加一个 pull case，覆盖 64 MiB、checksum failure 和断开后的 descriptor 失效；只运行一次该 target。

### Stage 3 — Push Slot Path

- 服务端固定 slot pool、lease state/generation、WRITE-ready-persist-release 流程。
- 不实现客户端 slot cache。
- 阶段验证：同一 contract target 增加一个 push case，覆盖并发争用、旧 generation 拒绝和 durable ACK；只运行一次该 target。

### Stage 4 — Preferred Transfer And Safe Fallback

- 在 `StorageTransferClient` 旁增加 additive preferred factory；现有 gRPC factory 不变。
- RDMA success 转换为现有 durable result；仅 `not_started/rejected_before_transfer` 调用 gRPC fallback；uncertain 不重发。
- `storage_node_app` 启动/关闭 RDMA server，`storage_client` 使用 preferred factory；现有未启用 RDMA 的命令行为保持不变。
- 阶段验证：一个 transfer integration target；最终追加一次相关 CTest 集合，不运行全仓 all group。

## Validation Budget

- 编码前仅执行 configure/dependency capability check，不先跑全仓 baseline。
- 每个 Stage 完成后运行一个与该阶段直接对应的 targeted test command；失败必须修复，不允许 skip。
- 全部阶段完成后只运行一次：RDMA targets + `storage_upload_integration` + `storage_write_chunk_contract`。
- 不新增性能矩阵、长时间 stress、全平台实机矩阵或全仓 `all`；真实硬件只做 quickstart 中的一次 64 MiB smoke。

## Project Structure

### Documentation (this feature)

```text
specs/011-rdma-fast-path/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── checklists/
│   └── requirements.md
├── contracts/
│   ├── rdma-control-and-data.md
│   ├── durability-and-fallback.md
│   └── transport-selection.md
└── tasks.md
```

### Source Code (repository root)

```text
modules/store/
├── rdma/
│   ├── AGENTS.md
│   ├── module-notes.md
│   ├── rdma_transport.h
│   ├── rdma_transport.cpp
│   └── rdma_transport_linux.cpp
├── transfer/
│   ├── storage_transfer_client.h
│   └── storage_transfer_client.cpp
├── node/
├── chunk/
└── common/

apps/
├── storage_node_app.cpp
└── storage_client.cpp

tests/
├── rdma_transport_contract_test.cpp
├── storage_transfer_rdma_test.cpp
├── storage_upload_integration_test.cpp
└── CMakeLists.txt

CMakeLists.txt
```

**Structure Decision**: 新 `modules/store/rdma` 是必要的平台/transport 隔离边界，但保持一个 public header 和两个 implementation files。durability 逻辑不进入该模块；它只把完整 chunk 交给现有 `ChunkStore`。`proto/`、`modules/raft/`、manifest 格式和既有 gRPC service 不修改。

## Post-Design Constitution Check

- Verified core: **PASS** — 所有成功写仍走现有 ChunkStore 与 CommitObject。
- Protocol/persistence: **PASS** — 新 wire 仅属于内部 RDMA transport v1，不修改现有 proto 或 persisted bytes。
- Durability/restart: **PASS** — durable ACK 来自现有 publish 结果；restart 丢弃 RAM slots 并递增 generation。
- Cross-platform: **PASS** — portable unavailable implementation 明确返回状态并保留 gRPC。
- Minimal surface: **PASS** — 一个新模块、两个新测试文件、两个现有 adapter/app 接点；slot cache、generic RPC runtime 与多 chunk RDMA concurrency 延期。

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|---|---|---|
| 新增 `modules/store/rdma` 平台模块 | verbs headers、CM/QP/MR/CQ 生命周期必须隔离，且需要 non-Linux unavailable 实现 | 把 verbs 直接写入 transfer/node 会污染共享业务路径并扩大平台条件编译范围 |
| additive preferred transfer factory | 必须在不改变既有 gRPC factory 行为的情况下表达 RDMA-first/fallback | 直接改 `CreateGrpcStorageTransferClient()` 会让名称和旧调用契约失真 |
