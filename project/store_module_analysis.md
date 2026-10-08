# Store 模块分析

> 撰写日期：2026-07-13  
> 对应源码：`modules/store/`

---

## 一、模块定位

`modules/store/` 是 **StorageNode / chunk data-plane** 的主目录，负责对象存储系统中数据面的核心职责：

- **Chunk 生命周期管理**：写入、读取、删除、校验
- **本地磁盘持久化**：基于 durable file 语义的可靠 IO
- **内存索引**：chunk 元数据的分片内存索引
- **放置策略**：副本选择与候选节点决策
- **数据传输**：跨 StorageNode 的 chunk 读写、对象上传/下载编排
- **后台维护**：GC、scrub、repair、rebalance 等维护任务基础设施
- **RPC 适配**：gRPC service/client 层

> 它与 `modules/raft`（控制面 / Raft 一致性内核）和 `modules/view`（集群拓扑发现）协同工作，构成完整的对象存储系统。

---

## 二、整体架构

```
┌────────────────────────────────────────────────────────┐
│                    StorageNode                          │
│  ┌──────────┐  ┌────────────┐  ┌──────────────────┐   │
│  │ gRPC     │  │ Upload     │  │ Maintenance      │   │
│  │ Service  │──│ Coordinator│  │ (GC/Scrub/Repair │   │
│  │ (node/)  │  │ (upload/)  │  │  /Rebalance)     │   │
│  └────┬─────┘  └─────┬──────┘  └────────┬─────────┘   │
│       │              │                  │              │
│       └──────────────┼──────────────────┘              │
│                      ▼                                 │
│  ┌────────────────────────────────────────────┐        │
│  │              ChunkStore (chunk/)            │        │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  │        │
│  │  │ Durable  │  │ Chunk    │  │ Chunk    │  │        │
│  │  │ File IO  │  │ Index    │  │ Index    │  │        │
│  │  │ (io/)    │  │ (index/) │  │ (memory) │  │        │
│  │  └──────────┘  └──────────┘  └──────────┘  │        │
│  └────────────────────────────────────────────┘        │
│                                                         │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │ Placement    │  │ Transfer     │  │ Runtime      │  │
│  │ (placement/ )│  │ (transfer/ ) │  │ (runtime/ )  │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
└────────────────────────────────────────────────────────┘
```

---

## 三、子模块详解

### 3.1 `common/` — 基础类型定义

**文件**：`store_types.h` / `store_types.cpp`

定义整个 store 模块共享的基础类型和工具函数：

| 分类 | 主要内容 |
|------|----------|
| **ID 类型** | `StorageNodeId`（string）、`ChunkId`（string） |
| **状态码** | `StorageNodeStatusCode` — 14 种状态，如 `kOk`、`kNotFound`、`kChecksumMismatch`、`kDiskFull` |
| **Chunk 状态** | `ChunkState` — `kStaging` → `kLive` → `kDeleting` → `kDeleted` / `kQuarantined` / `kCorrupted` / `kMissing` |
| **校验和** | `ChunkChecksum` — 算法类型（当前支持 SHA-256）、摘要值、大小、计算时间 |
| **Chunk 标识** | `ChunkIdentity` — 包含 chunk_id、object_id、version、chunk_index、offset |
| **元数据** | `ChunkMetadata` — chunk 完整元数据（标识、节点、大小、校验和、状态、时间戳） |
| **索引条目** | `ChunkIndexEntry` — 内存索引条目，包含路径信息（final_path、staging_path）和锁分片 |
| **工具函数** | `MakeChunkId()`、`ParseChunkId()`、`ComputeChunkChecksum()`、`VerifyChunkChecksum()` |

关键常量：
- 生产 chunk 大小：`kProductionChunkSizeBytes = 128 MiB`
- Chunk ID 分隔符：`'~'`
- SHA-256 摘要：32 bytes / 64 hex chars

---

### 3.2 `chunk/` — Chunk 存储抽象与本地实现

**文件**：`chunk_store.h`、`local_disk_chunk_store.h` / `.cpp`

#### ChunkStore 抽象接口

定义 data-plane 的核心操作契约：

| 操作 | 描述 |
|------|------|
| `WriteChunk` | 写入 chunk 数据到 staging 并发布到 final 路径 |
| `ReadChunk` | 读取 chunk 数据，支持范围读取和校验和验证 |
| `DeleteChunk` | 删除 chunk 数据，标记为删除状态 |
| `StatChunk` | 查询 chunk 元数据，可选校验和验证 |
| `ListChunks` | 分页列出 chunk，支持状态过滤、前缀匹配 |

请求/响应结构体完备，包含幂等 request_id、校验和验证、错误分类、重试建议等字段。

#### LocalDiskChunkStore

本地磁盘实现，依赖以下组件协作：
- **DurableFile**：可靠的磁盘 IO
- **ChunkIndex**：分片内存索引
- **BoundedStorageExecutor**：有界任务调度

目录布局：
```
<data_dir>/
├── live/         # 已发布的 chunk 数据
├── staging/      # 写入中（未发布）的 chunk 数据
└── quarantine/   # 隔离的损坏 chunk
```

---

### 3.3 `io/` — 持久化文件 IO

**文件**：`durable_file.h` / `durable_file.cpp`

提供平台感知的持久化文件抽象，确保写入的数据在系统崩溃后仍然可恢复。

#### 错误码体系

`DurableFileErrorCode` — 13 种错误，包括 `kDiskFull`、`kChecksumMismatch`、`kPartialWrite`、`kAtomicPublishFailed`、`kDirectorySyncFailed` 等。

从 `DurableFileErrorCode` 到 `StorageNodeStatusCode` 的映射通过 `MapDurableFileErrorCode()` 完成。

#### 核心抽象

| 接口 | 职责 |
|------|------|
| `DurableFile` | 基础抽象：路径规范化、staging writer 打开、文件发布、目录同步 |
| `DurableFileWriter` | 写入器：Append、Flush（数据/元数据）、Close |
| `LinuxDurableFile` | Linux 实现：使用 `O_DIRECT`、`fdatasync()`、`rename()` 原子发布 |
| `WindowsDurableFile` | Windows 实现（预留骨架） |

#### 关键操作

- **原子发布**：通过 `rename()` 系统调用实现 staging → final 的原子切换
- **目录同步**：发布后对父目录执行 `fsync()` 确保目录元数据持久化
- **路径构建**：`BuildChunkPathLayout()` 根据 chunk_id 和 staging_token 生成 staging/final 路径

---

### 3.4 `index/` — Chunk 内存索引

**文件**：`chunk_index.h` / `chunk_index.cpp`

#### ChunkIndex 抽象接口

| 操作 | 描述 |
|------|------|
| `Insert` | 插入索引条目 |
| `Update` | 更新已有条目 |
| `Find` | 通过 chunk_id 查找 |
| `Remove` | 删除条目 |
| `List` | 分页列出，支持状态过滤和前缀匹配 |
| `AcquireChunkLock` | 获取 chunk 级互斥锁 |

#### ShardedChunkIndex 实现

- **分片设计**：默认 64 个 shard，256 个锁 stripe
- **锁粒度**：基于 chunk_id 哈希的 stripe 级互斥锁（`ChunkLockGuard`）
- **分页**：默认每页 128 条，上限 4096 条
- **快照纪元**：每次变更递增 `mutation_epoch_`，支持快照一致性视图

---

### 3.5 `runtime/` — 有界任务执行器

**文件**：`storage_executor.h` / `.cpp`，`module-notes.md`

#### BoundedStorageExecutor

用于 store data-plane 的线程池执行器：

| 特性 | 说明 |
|------|------|
| 固定 worker | 配置指定（默认 4），启动后固定 |
| 有界队列 | 队列满时立即返回 overloaded，不阻塞 |
| 停机语义 | `kDrain`（排空后停） / `kCancelPending`（丢弃未执行任务后停） |
| 异常安全 | worker 捕获异常继续存活 |
| 可观测性 | 统计 submitted / completed / rejected / failed / dropped |

核心约束：
- 只做运行时调度，不引入 chunk/IO 业务编排
- 队列必须保持有界
- 析构按 drain 语义回收

---

### 3.6 `node/` — StorageNode RPC 适配

**文件**：`storage_node_service.h` / `.cpp`、`storage_node_client.h` / `.cpp`、`storage_node_registry.h` / `.cpp`

#### StorageNodeService

基于 gRPC CallbackService 的 data-plane 服务端，实现 `storage::StorageNodeService` 接口：

| RPC | 对应操作 |
|-----|----------|
| `WriteChunk` | 写入 chunk |
| `ReadChunk` | 读取 chunk |
| `DeleteChunk` | 删除 chunk |
| `ScrubChunk` | 巡检 chunk |
| `RepairChunk` | 修复 chunk |
| `BatchDeleteChunks` | 批量删除 chunk |
| `RegisterStorageNode` | 节点注册 |
| `UpdateStorageNodeHeartbeat` | 心跳更新 |
| `ReportHealth` | 健康状态报告 |
| `ReportCapacity` | 容量报告 |
| `ReportLoad` | 负载报告 |

#### StorageNodeClient

客户端封装，提供：
- 带重试的写入（`max_write_retries` 配置）
- 副本回退读取（`ReadChunkWithReplicaFallback`）
- 批处理删除
- 注册/心跳/健康/容量/负载报告

关键函数：`ReadChunkWithReplicaFallback()` — 优雅处理副本读取失败，依次尝试副本节点。

#### StorageNodeRegistry

内存中的 StorageNode 信息注册表，维护每个节点的：

| 信息 | 说明 |
|------|------|
| 基本事实 | endpoint、incarnation_id、sequence |
| 容量事实 | 总容量、已用容量、可用容量、chunk 数 |
| 健康事实 | 健康状态、磁盘压力、IO 错误数、可写性 |
| 负载事实 | 活跃读/写数、排队操作数、过载标志 |
| 存活状态 | Live（活跃）/ Stale（超时）/ Dead（死亡） |
| 失效域 | zone、rack |

配置项：
- `stale_timeout_ms`：默认 30s
- `dead_timeout_ms`：默认 90s
- `enforce_unique_endpoints`：检查 endpoint 唯一性

---

### 3.7 `placement/` — 放置策略

**文件**：`placement_manager.h` / `.cpp`、`replica_policy.h` / `.cpp`

#### 副本策略（ReplicaPolicy）

| 参数 | 说明 |
|------|------|
| `replica_count` | 期望副本数（默认 3） |
| `minimum_successful_writes` | 最小成功写入数（默认 2） |
| `avoid_same_node` | 避免选择同一节点 |
| `prefer_distinct_zones` | 倾向不同可用区 |
| `reserve_capacity_bytes` | 预留容量 |

#### 放置决策（PlacementManager）

支持多种候选节点来源：
- `StorageNodeRegistry` 快照
- `ViewNodeBackedStorageNodeSnapshot`（来自 ViewNode 的观测）
- `ViewNodeRegistry`（直接集成 ViewNode）
- `DiscoverStorageResult`

#### 读取副本选择（ReadReplicaSelection）

- 根据健康、负载、延迟、损坏状态对副本排序
- 支持排除特定节点
- 返回有序的候选列表用于回退读取

#### 候选节点结构（StorageNodePlacementCandidate）

包含节点 ID、endpoint、健康、磁盘压力、容量、负载、失效域等信息，提供 `CanFit()` 和 `HasWritableHealth()` 方法。

---

### 3.8 `upload/` — 上传协调

**文件**：`upload_coordinator.h` / `.cpp`

#### UploadCoordinator

协调 chunk 写入的完整流程：

1. **metadata create**：通过 `UploadMetadataClient` 创建 PENDING 对象
2. **placement 决策**：使用 `PlacementManager` 选择副本节点
3. **chunk 写入**：通过 `UploadChunkWriter` 向每个副本节点写入
4. **metadata commit**：提交 COMMITTED manifest

关键限制：
- payload 只能是单个 bounded chunk 的 data-plane buffer
- 不在 coordinator 内拼接整对象
- 不负责 metadata 底层实现或 StorageNode RPC server

#### UploadCoordinatorRequest

包含完整的上传上下文：request_id、bucket/object_key/object_id、version、chunks 列表、副本策略、候选节点、排除节点等。

#### 结果分类

- `committed`：所有 chunk 写入成功且 metadata 已提交
- `pending_object_possible`：可能存在 PENDING 对象
- `orphan_chunk_possible`：可能存在孤立的 chunk

---

### 3.9 `transfer/` — 数据传输适配

**文件**：`object_transfer.h` / `.cpp`、`metadata_transfer_client.h` / `.cpp`、`storage_transfer_client.h` / `.cpp`

#### ObjectTransfer — 对象传输编排

定义 upload 和 download 的完整流程和协议：

| 方向 | 流程 |
|------|------|
| **Upload** | Preparing → DiscoveringMetadata → PlanningWrite → UploadingChunks → CommittingObject → VerifyingChecksums → Completed |
| **Download** | Preparing → FetchingManifest → DownloadingChunks → VerifyingChecksums → Completed |

核心数据结构：
- `TransferWritePlan`：来自 MetadataNode 的写入计划
- `TransferCommittedManifest`：COMMITTED 状态的 manifest
- `TransferChunkPlan`：单个 chunk 的写入计划
- `TransferPreparedChunk`：本地准备的 chunk facts
- `TransferSessionSnapshot`：传输过程全状态快照

#### MetadataTransferClient

transfer → MetadataService 的适配边界：

| 操作 | 说明 |
|------|------|
| `CreateWritePlan` | 申请写入计划（返回 metadata/control-plane facts） |
| `CommitObject` | 提交 chunk manifest |
| `HeadObject` | 查询对象概要 |
| `GetObjectManifest` | 获取 COMMITTED manifest |

**重要**：只定义适配边界，不实现 RPC 调用逻辑、ViewNode discovery、不保存权威 manifest。

#### StorageTransferClient

transfer → StorageNode data-plane 的适配边界：

| 操作 | 说明 |
|------|------|
| `WriteChunk` | 对单个 StorageNode 发起 chunk 写入 |
| `ReadChunk` | 对单个 StorageNode 发起 chunk 读取 |

提供 `CreateGrpcStorageTransferClient()` 工厂函数返回基于 gRPC 的实现。

---

### 3.10 `maintenance/` — 后台维护任务

**文件**：`garbage_collector.h` / `.cpp`、`gc_task_store.h` / `.cpp`、`scrub_manager.h` / `.cpp`、`repair_manager.h` / `.cpp`、`rebalance_manager.h` / `.cpp`

#### GarbageCollector — 垃圾回收

| 特性 | 说明 |
|------|------|
| 清理来源 | 已删除对象、PENDING 超时、失败上传、ABORT 清理 |
| 任务管理 | 提交、排队、重试、完成、失败、取消 |
| 安全检测 | metadata-driven safety checker 决定是否允许删除 |
| 持久化 | `GarbageCollectorTaskStore` 支持任务快照持久化 |
| 统计 | 全生命周期统计（submitted/rejected/completed/failed） |

#### ScrubManager — 数据巡检

| 特性 | 说明 |
|------|------|
| 任务类型 | 对指定 chunk 的所有副本进行校验和验证 |
| 结果分类 | 正常 / 损坏 / 缺失 / 隔离 |
| 修复候选 | 检测到副本不足或损坏时生成 `ScrubRepairCandidate` |
| 停机模式 | Drain / CancelPending |

#### RepairManager — 副本修复

| 特性 | 说明 |
|------|------|
| 触发方式 | 手动提交修复任务 / 由 Scrub 结果自动触发 |
| 修复流程 | 从健康源节点读取 → 校验 checksum → 写入目标节点 |
| 任务状态 | Queued → Running → Completed / Failed / Cancelled / RetryPending |
| 并发控制 | 最大活跃任务数、最大总任务数 |

#### RebalanceManager — 数据重平衡

| 特性 | 说明 |
|------|------|
| 触发原因 | 容量不均衡、热点、新节点加入、节点排空、维护 |
| 完整流程 | 源读取 → 校验 → 目标写入 → 校验 → manifest 协调 → 源清理 |
| 幂等处理 | 支持目标已存在、manifest 已应用、源已缺失等场景 |
| 孤儿候选 | 传输失败时记录清理候选，避免数据丢失 |

---

## 四、模块间依赖关系

```
store/
├── common/          ← 无内部依赖，被所有子模块引用
├── io/              ← 依赖 common/
├── index/           ← 依赖 common/
├── runtime/         ← 依赖 common/
├── chunk/           ← 依赖 common/、io/、index/、runtime/
├── placement/       ← 依赖 common/、node/ (registry)
├── upload/          ← 依赖 chunk/、placement/、runtime/、metadata_client (外部)
├── transfer/        ← 依赖 chunk/、maintenance/ (gc)、metadata/ (raft)、runtime/
├── node/            ← 依赖 chunk/、runtime/、proto (storage_node.grpc.pb.h)
└── maintenance/     ← 依赖 chunk/、io/、placement/、runtime/、node/ (registry)
```

**外部依赖**：
- `proto/` — 所有 gRPC 服务定义
- `modules/raft/metadata/` — MetadataTransferClient 依赖的 metadata records
- `modules/view/` — ViewNode 相关的发现和注册接口
- `modules/cluster/` — 集群配置

---

## 五、关键设计决策

### 5.1 有界队列

所有队列（runtime 执行器、maintenance 任务队列）都采用有界设计，满时立即返回 overloaded，避免无限积压导致内存爆炸。

### 5.2 原子发布

chunk 写入采用 staging → publish 的两阶段写入：
1. 先写入 staging 目录
2. 发布时用 `rename()` 原子切换到 final 目录
3. 发布后同步父目录确保持久化

### 5.3 副本回退读取

读取操作支持副本回退机制：优先选择健康副本，遇到失败时依次尝试其他副本，而不是立即返回错误。

### 5.4 幂等设计

所有关键操作（写入、删除、注册、心跳等）都设计为幂等的，通过 request_id 和 incantation_id 实现重试安全。

### 5.5 平台持久化语义

- Linux 使用 `O_DIRECT` + `fdatasync()` + 目录 `fsync()`
- Windows 提供等价接口骨架
- 不允许 no-op 实现静默降级

### 5.6 分层清晰的职责边界

- `common/` 不做文件 IO、不做 RPC
- `io/` 只处理 durable file 语义，不承接 chunk 业务编排
- `index/` 只维护本地索引
- `runtime/` 只负责本地任务调度
- `node/` 负责 RPC 适配和注册表
- `placement/` 只做决策，不发起写入
- `upload/` 只做协调，不负责 metadata 底层实现
- `maintenance/` 只做后台任务模型，不决定对象可见性

---

## 六、测试覆盖

store 模块的相关测试分布在 `tests/` 目录下：

| 测试文件 | 覆盖内容 |
|----------|----------|
| `local_disk_chunk_store_test.cpp` | LocalDiskChunkStore 的读写删验证 |
| `storage_client_config_test.cpp` | StorageNodeClient 配置验证 |
| `metadata_*_test.cpp` | 元数据相关测试（与 store 交互） |
| `integrated_object_storage_*_test.cpp` | 集成测试（e2e、并发、quorum、恢复） |

---

## 七、总结

`modules/store/` 是一个结构清晰、职责分明的数据面存储模块，涵盖了从底层磁盘 IO 到上层数据编排的完整链路。它与 Raft 控制面（`modules/raft/`）和集群视图（`modules/view/`）协同工作，构成了一个完整的分布式对象存储系统的数据面基础设施。

当前模块已实现的核心能力：
- ✅ 本地磁盘 chunk 存储与索引
- ✅ 平台感知的持久化文件 IO
- ✅ gRPC 数据面 service/client
- ✅ 节点注册、心跳、健康/容量/负载报告
- ✅ 副本放置策略
- ✅ 上传协调基础框架
- ✅ 跨节点数据传输适配
- ✅ GC / Scrub / Repair / Rebalance 后台维护任务框架
