# Feature Specification: RDMA Fast Path

**Feature Branch**: `011-rdma-fast-path`  
**Created**: 2026-10-08  
**Status**: Draft  
**Input**: User description: "在现有对象存储旁增加 RDMA 优先、现有传输可回退的 Fast Path；控制与大块数据传输分离，分阶段完成，保持现有一致性、持久化和可见性语义，并减少非必要的编码前验证与写后测试。"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - 建立最小 RDMA 通道 (Priority: P1)

作为对象存储开发者，我希望客户端与 StorageNode 能通过一条最小、独立的 RDMA 通道完成小型请求/响应，以确认连接、消息边界、资源清理和请求分发都可工作，同时不替换现有传输系统。

**Why this priority**: 后续大块数据搬运和回退策略都依赖一个可独立验证的连接与完成通知边界；先交付这一阶段可以用最小风险确认环境和调用链。

**Independent Test**: 在支持 RDMA 的 Linux 环境中启动一个客户端和一个 StorageNode，完成一次小型探测请求及响应，并验证正常关闭和失败清理；不要求上传完整对象。

**Acceptance Scenarios**:

1. **Given** 客户端与 StorageNode 均具备可用 RDMA 能力，**When** 客户端发起小型探测请求，**Then** StorageNode 返回匹配响应且连接资源可正常释放。
2. **Given** 连接协商失败或节点不具备 RDMA 能力，**When** 客户端尝试建立 RDMA 通道，**Then** 系统返回可分类的“RDMA 未开始”结果，不影响现有传输路径继续使用。
3. **Given** 请求内容无效或响应无法解析，**When** 通道处理该请求，**Then** 系统显式失败并保留诊断信息，不报告伪成功。

---

### User Story 2 - 单 Chunk RDMA 数据传输 (Priority: P2)

作为对象存储客户端，我希望单个 chunk 的大块 payload 通过 RDMA 数据面传输，而对象元数据、校验事实和提交控制仍沿用既有控制边界，从而减少大块数据经过普通请求载荷的开销。

**Why this priority**: 单 chunk 是可控且可独立验证的数据面增量，能够在不引入整对象并发编排的情况下确认 buffer 所有权、生命周期和持久化完成语义。

**Independent Test**: 上传一个单 chunk 对象，经 RDMA 完成数据搬运后等待 StorageNode 的持久化确认，再通过既有读取路径取回并校验内容；测试内存占用不随对象之外的数据增长。

**Acceptance Scenarios**:

1. **Given** 客户端持有有效的注册缓冲区且 StorageNode 可发起读取，**When** 客户端提交单 chunk descriptor，**Then** StorageNode 完成数据读取、checksum 校验和真实持久化后才返回 durable success。
2. **Given** StorageNode 提供由其权威分配的可写 slot，**When** 客户端完成单 chunk 写入并通知 slot ready，**Then** StorageNode 校验、持久化并释放 slot，客户端不能仅凭网络完成判定上传成功。
3. **Given** descriptor 已过期、连接已重建或 StorageNode 已重启，**When** 客户端尝试复用旧 descriptor，**Then** 请求被明确拒绝且旧地址、key 或 slot 不被继续使用。
4. **Given** chunk 内容与期望 checksum 不一致，**When** StorageNode 验证数据，**Then** chunk 不得进入 durable success 或对象提交清单。

---

### User Story 3 - 安全回退与现有上传集成 (Priority: P3)

作为对象存储用户，我希望上传默认优先尝试 RDMA，并在能够确认远端尚未接收或提交数据时自动使用现有传输路径；若远端状态不确定，则停止盲目重发并返回可恢复、可诊断的结果。

**Why this priority**: Fast Path 只有在不破坏既有上传可用性、幂等边界、持久化保证和最终提交可见性时才可投入使用。

**Independent Test**: 分别模拟 RDMA 不可用、连接建立失败、传输前失败、持久化成功和结果不确定五类情况，确认只有明确安全的失败发生回退，且最终 manifest 仅包含真实 durable replica facts。

**Acceptance Scenarios**:

1. **Given** RDMA 不可用、连接建立失败或数据明确尚未提交远端，**When** 客户端上传 chunk，**Then** 自动切换到现有传输路径并复用同一 chunk identity 与幂等请求事实。
2. **Given** RDMA 数据可能已到达远端但完成状态未知，**When** 客户端无法获得最终持久化确认，**Then** 系统不得盲目通过现有路径重复写入，而是返回“结果不确定”并保留恢复诊断。
3. **Given** StorageNode 已返回真实持久化成功，**When** 上传流程汇总结果，**Then** 仅使用实际 durable 节点生成 chunk manifest，并继续以既有 CommitObject 作为对象可见性边界。
4. **Given** RDMA 路径完全关闭，**When** 执行既有上传与下载，**Then** 其外部行为与当前稳定基线一致。

## Current Baseline & Scope Boundaries *(mandatory)*

### Existing Baseline

- 当前对象上传由客户端传输编排层获取 write plan，向 StorageNode 写入单个 bounded chunk，并以真实 durable 写结果生成 manifest facts。
- 同一 chunk 的可重试写入复用 request identity 与 chunk identity；StorageNode 的 durable success 必须跨过现有 publish 边界。
- CommitObject 是对象对外可见的唯一最终边界；payload 不进入 Metadata、Raft log、Raft snapshot 或 metadata snapshot。
- 现有 gRPC 数据传输路径、manifest-driven download、checksum 校验和 bounded-memory 约束均保留，且不在本特性中重写。

### Targeted Gaps Or Risks

- 现有数据路径会让大块 chunk payload 经普通 RPC 消息搬运，缺少可选的 RDMA Fast Path。
- RDMA buffer、remote descriptor 和 slot ownership 若没有明确生命周期，可能导致 use-after-free、跨重启复用和多客户端争用。
- 网络完成与磁盘持久化完成存在语义差异，错误合并可能提前报告成功。
- 在 ACK 丢失或远端状态不确定时无条件 fallback，可能放大重复写或破坏幂等诊断。

### Non-Goals

- 不引入 SPDK、NVMe-oF、共享 CQ、复杂 poller/event-loop、优先级队列、批处理、selective signaling、复杂异步 runtime 或动态多尺寸 MR pool。
- 不把 Metadata、Raft、View 或全部 RPC 强制替换为 RDMA，也不改变既有协议语义、持久化格式、公共 API 行为、类名、函数名或命名空间。
- 不在第一版实现整对象并发 RDMA、零拷贝落盘、跨节点 repair/rebalance、复杂性能自适应或生产级多租户资源治理。
- 不让业务编排层直接操作底层 RDMA 资源细节；不把 chunk payload 放入控制面消息。

### Platform Scope

- Linux 是第一阶段真实 RDMA 构建与运行验证平台；缺少 RDMA 硬件时允许使用明确标记的可控测试替身验证状态边界。
- Windows/macOS 或未提供 RDMA 能力的平台必须明确报告 RDMA 不可用并继续使用现有传输路径，不允许以 no-op 冒充 RDMA 成功。
- 共享业务路径保持跨平台；平台专属资源管理隔离在 RDMA transport 边界内。

### Edge Cases

- 客户端 buffer 在远端读取完成前被释放或复用时，传输必须失败，不能提交对象。
- 多个客户端竞争同一 remote slot 时，以 StorageNode 的 ownership 结果为准；客户端缓存不能授予所有权。
- StorageNode 重启、连接重建或 generation 变化后，所有旧 descriptor 与 slot cache 必须失效。
- RDMA completion 已发生但 durable ACK 丢失时，结果进入“不确定”状态，不得直接 fallback 重发。
- chunk 已 durable 但 CommitObject 失败时，继续沿用现有 pending/orphan cleanup 语义，不改变可见性边界。
- RDMA 初始化、注册内存或运行时资源不足时，若确认数据尚未提交，可安全回退；否则显式返回失败或不确定状态。

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: 系统 MUST 在不移除现有传输路径的前提下，为客户端到 StorageNode 提供可选的 RDMA Fast Path。
- **FR-002**: 系统 MUST 将小型控制事实与大块 chunk payload 分离，控制面不得承载完整 chunk payload。
- **FR-003**: 系统 MUST 提供最小请求/响应通道，用于验证连接、请求分发、响应匹配和资源清理；第一版不得依赖复杂通用 RPC runtime。
- **FR-004**: 系统 MUST 支持由客户端暴露 buffer、StorageNode 拉取数据的单 chunk 模式，并在远端读取完成前保持客户端 buffer 有效。
- **FR-005**: 系统 MUST 支持由 StorageNode 权威分配预注册 slot、客户端写入、StorageNode 持久化后释放的单 chunk 模式。
- **FR-006**: StorageNode MUST 是 slot ownership、状态与 generation 的最终权威；客户端 slot map 只能作为可失效缓存。
- **FR-007**: 连接重建、StorageNode 重启或 generation 变化 MUST 使旧 descriptor、地址、key 和 slot cache 明确失效。
- **FR-008**: 系统 MUST 区分网络传输完成与本地持久化完成；只有现有 durable boundary 成功后才能报告 durable write success。
- **FR-009**: 系统 MUST 在 RDMA 不可用、连接建立失败或明确未发生远端提交时允许 fallback 到现有传输路径。
- **FR-010**: 系统 MUST 在远端到达或持久化状态不确定时禁止盲目 fallback，并返回可诊断的“不确定”结果。
- **FR-011**: 安全 fallback 和同一 chunk 的重试 MUST 复用既有 chunk identity 与幂等请求事实，不产生新的对象可见性规则。
- **FR-012**: CommitObject MUST 继续作为唯一对象可见性边界，manifest MUST 只记录实际 durable replica nodes，payload MUST 不进入 Metadata 或 Raft。
- **FR-013**: RDMA 关闭或平台不支持 RDMA 时，现有上传、下载、checksum 和错误语义 MUST 保持不变。
- **FR-014**: Linux 真实 RDMA 路径 MUST 显式验证资源创建与释放；非 Linux 分支 MUST 返回明确 unsupported 并走现有路径，不得 no-op 成功。
- **FR-015**: 第一阶段开发验证 MUST 聚焦阶段契约：每阶段只保留一个最小 smoke/contract 验证和必要的既有回归，不展开大规模组合矩阵或全仓重复验证。
- **FR-016**: 第一阶段写后测试 MUST 采用“受影响目标构建 + 对应阶段 targeted tests + 一次最终相关测试组”的最小集合；不得跳过失败测试，也不得用减少测试改变 durability 或协议安全标准。

### Key Entities

- **RDMA Connection**: 客户端与 StorageNode 之间的短生命周期连接，包含 capability、generation、completion 和清理状态。
- **Remote Buffer Descriptor**: 一次受限数据操作所需的远端地址、访问 key、容量、generation 与有效期事实；不能跨连接或重启继续使用。
- **Slot Lease**: StorageNode 权威授予的临时 slot 使用权，具有 ownership、容量、状态、generation 和释放结果。
- **Chunk Transfer Attempt**: 某个 chunk 在 RDMA 或现有路径上的一次传输尝试，关联 request identity、chunk identity、传输阶段和结果确定性。
- **Chunk Transfer Outcome**: 汇总网络完成、checksum、durability、fallback eligibility 和诊断信息的结果；只有 durable outcome 能进入 manifest。

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 在支持 RDMA 的 Linux 环境中，最小请求/响应场景连续执行 100 次均返回匹配结果，且连接关闭后无遗留 active connection 或 buffer lease。
- **SC-002**: 两种单 chunk 数据模式各能完成至少 1 个不小于 64 MiB 的 chunk 传输，读取校验结果与源数据完全一致，且对象仅在 durable confirmation 后可提交。
- **SC-003**: 对 RDMA 不可用、连接失败、明确未提交、结果不确定和 durable success 五类结果，100% 按定义执行 fallback、停止或提交，不出现不确定状态下的盲目重复写。
- **SC-004**: StorageNode 重启或连接 generation 变化后，旧 descriptor/slot 的复用尝试 100% 被拒绝，且新连接可重新获取有效资源。
- **SC-005**: RDMA 关闭或不受支持时，现有上传/下载 targeted regression 全部通过，且外部可见的 CommitObject、manifest 和 checksum 行为无变化。
- **SC-006**: 每个实现阶段的强制验证不超过一个阶段级 targeted test 命令，最终只追加一次相关测试组验证；任何失败均被处理而非跳过。

## Assumptions

- 第一阶段以单客户端、单连接、单 chunk 为主，先完成清晰的同步/专用 completion 处理，不追求峰值并发性能。
- 两种数据模式按阶段交付：先验证客户端 buffer 被远端读取，再验证 StorageNode slot pool；两者共享同一 durability 和结果分类规则。
- 现有 chunk identity 与重复写语义足以承载安全重试；若源码核查发现冲突，必须在实现前回到规范澄清，而不是改变协议或持久化格式。
- RDMA 依赖和真实硬件可用性由构建环境提供；无硬件环境只验证状态机和 fallback，真实硬件 smoke test 作为 Linux 接受条件。
- 为控制任务规模，计划采用少量阶段性任务，每个阶段同时完成该阶段的实现、最小契约验证和文档更新，而不是拆成大量微任务。
