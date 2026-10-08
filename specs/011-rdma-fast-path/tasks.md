# Tasks: RDMA Fast Path

**Input**: Design documents from `/specs/011-rdma-fast-path/`  
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/, quickstart.md

**Tests**: 按用户要求保留最小测试集合。每个阶段只安排一个 targeted test task/command，最终只运行一次相关回归集合；任何失败不得跳过。

**Organization**: 按 User Story 顺序一次完成一个阶段。任务不拆成逐函数微任务；每个阶段完成实现、最小验证和必要文档后再进入下一阶段。

## Format: `[ID] [P?] [Story] Description`

- **[P]**: 可与同阶段其他任务并行，且不写同一文件
- **[Story]**: 对应 spec.md 的 User Story
- 所有任务均给出具体文件路径

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: 建立可选 RDMA 构建与最小测试入口，不改变现有 runtime 行为。

- [ ] T001 在 `CMakeLists.txt` 实现 `CQUPT_RDMA_MODE=AUTO|ON|OFF`、rdma-core 探测、真实/portable source 选择，并在 `tests/CMakeLists.txt` 增加两个 RDMA test targets；`ON` 缺依赖必须 configure 失败
- [ ] T002 [P] 在 `modules/store/rdma/AGENTS.md` 与 `modules/store/rdma/module-notes.md` 记录模块边界、非 Linux unsupported、durable ACK 和 no-generic-runtime 规则

**Checkpoint**: AUTO/OFF 能在无 RDMA 环境 configure；ON 的失败行为明确；尚未改变上传路径。

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: 提供所有 story 共用的窄 transport contract 与 portable unavailable 行为。

**⚠️ CRITICAL**: 完成本阶段后才能进入连接、Pull/Push 和 fallback。

- [ ] T003 在 `modules/store/rdma/rdma_transport.h` 定义最小 client/server config、attempt state、result 与 factories，并在 `modules/store/rdma/rdma_transport.cpp` 实现 endpoint+port-offset 校验和 portable unavailable；禁止把 verbs 头或复杂业务逻辑放入 `.h`

**Checkpoint**: non-Linux/RDMA-disabled 路径只能返回明确 unavailable/not_started，不能伪成功。

---

## Phase 3: User Story 1 - 建立最小 RDMA 通道 (Priority: P1) 🎯 MVP

**Goal**: 单客户端与单 StorageNode 完成最小 probe request/response、generation 校验和确定性资源清理。

**Independent Test**: `rdma_transport_contract` 在无设备时断言明确 unavailable；在可用 Linux RDMA 环境连续 probe 并正常关闭，不遗留 active connection。

### Minimal Test For User Story 1

- [ ] T004 [US1] 在 `tests/rdma_transport_contract_test.cpp` 添加 capability、probe、错误 version/generation 和 cleanup cases，先确认真实实现缺失时用例失败，再进入实现

### Implementation For User Story 1

- [ ] T005 [US1] 在 `modules/store/rdma/rdma_transport_linux.cpp` 实现最小 CM/QP/MR/CQ RAII、blocking SEND/RECV probe 和反向 cleanup；只保留一个 outstanding operation，不引入 pending map 或通用 RPC runtime

**Checkpoint**: 只运行 quickstart 定义的 `rdma_transport_contract` 命令并达到 PASS 后，才进入 User Story 2。

---

## Phase 4: User Story 2 - 单 Chunk RDMA 数据传输 (Priority: P2)

**Goal**: 依次完成 Pull 和 Push 单 chunk 数据面，并由既有 `ChunkStore::WriteChunk()` 给出唯一 durable success。

**Independent Test**: 一个至少 64 MiB 的 chunk 分别通过 Pull 与 Push 写入，读取 checksum 与源一致；旧 generation、slot 争用和 checksum mismatch 均显式失败。

### Minimal Test For User Story 2

- [ ] T006 [US2] 扩展 `tests/rdma_transport_contract_test.cpp`，集中加入 64 MiB Pull/Push、checksum failure、旧 descriptor/lease、slot ownership 和 durable-ACK-before-release cases，不新增第二套 fixture 文件

### Implementation For User Story 2

- [ ] T007 [US2] 在 `modules/store/rdma/rdma_transport_linux.cpp` 实现 client MR descriptor、server RDMA READ、bounded buffer 校验和调用 `ChunkStore::WriteChunk()` 的 Pull 路径，确保 durable response 前客户端 MR 不释放
- [ ] T008 [US2] 在 `modules/store/rdma/rdma_transport_linux.cpp` 实现固定 slot pool 的 FREE→RESERVED→WRITING→READY→FLUSHING→FREE 流程和 generation/owner 校验；每次重新申请 lease，不实现 client slot cache
- [ ] T009 [US2] 在 `apps/storage_node_app.cpp` 按现有 StorageNode endpoint 与可覆盖 port offset 启停 RDMA server，并保证启动失败可诊断、退出顺序先停 listener/connection 再销毁 `ChunkStore`

**Checkpoint**: 只运行一次更新后的 `rdma_transport_contract` 命令并达到 PASS 后，才进入 User Story 3。

---

## Phase 5: User Story 3 - 安全回退与现有上传集成 (Priority: P3)

**Goal**: 上传优先 RDMA，仅在远端数据操作明确未开始时 fallback，并保持现有 identity、durability、manifest 与 CommitObject 语义。

**Independent Test**: 对 not-started、pre-transfer reject、after-post uncertain、explicit failure、durable success 五类结果逐一验证 transport selection；uncertain 必须零次调用 fallback。

### Minimal Test For User Story 3

- [ ] T010 [US3] 在 `tests/storage_transfer_rdma_test.cpp` 用最小 fake RDMA/gRPC clients 覆盖五类 attempt state、相同 request/chunk identity、fallback 调用次数和 result mapping，先确认 preferred factory 尚未实现时用例失败

### Implementation For User Story 3

- [ ] T011 [US3] 在 `modules/store/transfer/storage_transfer_client.h` 添加 additive preferred config/factory 声明，并在 `modules/store/transfer/storage_transfer_client.cpp` 实现 RDMA-first 决策；保持 `CreateGrpcStorageTransferClient()` 和 `ReadChunk()` 行为不变，uncertain 禁止重发
- [ ] T012 [US3] 在 `apps/storage_client.cpp` 使用 preferred factory 和共享 RDMA port offset，RDMA disabled/unavailable 时保持现有 CLI 输出与 gRPC 上传行为
- [ ] T013 [US3] 在 `tests/storage_upload_integration_test.cpp` 增加一个单 chunk 集成回归，确认只有真实 durable RDMA/fallback result 进入 manifest，且 `CommitObject` 仍是对象可见性边界

**Checkpoint**: 只运行 `storage_transfer_rdma` 与受影响的单个 upload integration case；两者 PASS 后进入收尾。

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: 同步契约说明并执行一次最终相关回归，不扩展功能面。

- [ ] T014 [P] 更新 `modules/store/rdma/module-notes.md`、`modules/store/transfer/module-notes.md` 与 `specs/011-rdma-fast-path/quickstart.md`，记录最终 wire version、fallback matrix、Linux 实机范围和 Windows/macOS unsupported 行为
- [ ] T015 按 `specs/011-rdma-fast-path/quickstart.md` 执行一次受影响目标构建，以及 RDMA、`storage_upload_integration`、`storage_write_chunk_contract` 的最终 CTest 集合；严格按根 `AGENTS.md` 的精简日志规则报告结果

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1 Setup**: 无依赖；T001 与 T002 可并行。
- **Phase 2 Foundational**: 依赖 T001；阻塞所有 User Stories。
- **US1 / Phase 3**: 依赖 T003；先写最小 contract test，再实现 connection/probe。
- **US2 / Phase 4**: 依赖 US1；Pull 完成后再实现 Push，复用同一个 Linux transport 文件和 test target。
- **US3 / Phase 5**: 依赖 US2 的可用 write result；只在这一阶段接入 transfer/app/upload。
- **Phase 6 Polish**: 依赖计划交付的全部 User Stories。

### User Story Dependencies

```text
Setup → Foundation → US1 connection → US2 Pull/Push → US3 fallback/integration → Final validation
```

- **US1** 可独立演示连接/probe，不要求 chunk upload。
- **US2** 复用 US1 connection，但可直接对 `ChunkStore` 独立验证，不要求 ObjectTransfer/CommitObject。
- **US3** 复用 US2 result，通过 fake transport 独立验证 fallback matrix，再用一个 upload case 验证最终边界。

### Parallel Opportunities

- T002 可与 T001 并行，因为只写新模块文档。
- T014 可在 US3 实现稳定后与最终测试准备并行。
- 其余任务刻意顺序执行；它们集中修改相同 transport/transfer 文件，标记并行会增加冲突而没有实际收益。

## Parallel Example: Setup

```text
Task T001: 更新 CMakeLists.txt 与 tests/CMakeLists.txt
Task T002: 创建 modules/store/rdma/AGENTS.md 与 module-notes.md
```

## Implementation Strategy

### MVP First

1. 完成 Setup + Foundation。
2. 完成 US1 的 test → implementation。
3. 只运行一次 US1 targeted command。
4. 停止并确认最小 connection/probe 可交付，再决定是否进入 US2。

### One Stage At A Time

1. US1：连接与 probe。
2. US2：先 Pull，后 Push；完成后只做一次合并 contract 验证。
3. US3：fallback matrix 与现有上传接入。
4. 最终只做一次相关回归集合。

### Explicitly Deferred

- `google::protobuf::RpcChannel` generic runtime / generated generic Stub。
- 客户端 slot cache、shared CQ、selective signaling、batching、priority、多尺寸 MR pool。
- 多 chunk 并发 RDMA、read fast path、repair/rebalance RDMA。
- 全仓测试、长时间 stress、全平台 RDMA 实机矩阵。

## Notes

- 总任务数：15。
- User Story 任务数：US1=2，US2=4，US3=4。
- 测试任务数：4 个实现前/集成测试任务 + 1 个最终相关回归任务。
- 所有任务均遵循 checkbox、顺序 ID、必要的 Story label 和具体文件路径格式。
