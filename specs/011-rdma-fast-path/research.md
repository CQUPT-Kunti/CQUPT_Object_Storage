# Research: RDMA Fast Path

## Decision 1: Linux 使用 rdma-core 的 `librdmacm` 与 `libibverbs`

**Decision**: Linux 真实实现使用 rdma-core，连接管理走 `librdmacm`，数据操作与 MR/CQ 使用其 verbs 封装或 `libibverbs`。第一版采用同步/阻塞式 completion 或单一 handler，不建立自研异步 runtime。

**Rationale**: rdma-core 是 Linux RDMA userspace 的标准实现，官方示例提供最小 client/server ping-pong，且 `rdma_cm` 已覆盖连接、SEND/RECV、READ/WRITE 和 MR 注册边界。来源：[rdma-core](https://github.com/linux-rdma/rdma-core)、[rdma_cm overview](https://github.com/linux-rdma/rdma-core/blob/master/librdmacm/man/rdma_cm.7)、[minimal server example](https://github.com/linux-rdma/rdma-core/blob/master/librdmacm/examples/rdma_server.c)。

**Alternatives considered**:

- SPDK / FastBlock runtime：拒绝，超出当前线程模型与第一阶段范围。
- UCX 等更高层依赖：拒绝，新增依赖和抽象多于最小需求。
- 直接手写内核接口：拒绝，rdma-core 已提供稳定 userspace API。

## Decision 2: 不把 `google::protobuf::RpcChannel` 作为生产控制面基础

**Decision**: 第一版控制消息使用小型 versioned header，并复用现有 chunk request/response 字段语义；不引入 generic service Stub/runtime。`RpcChannel` 教学实验不进入本 feature 的生产任务。

**Rationale**: Protobuf 官方将 `service.h` generic RPC interfaces 标记为 deprecated，并建议具体 RPC 系统使用自己的 codegen plugin；现有仓库已经使用 gRPC stubs，强行开启 `cc_generic_services` 会并存第二套 service 层并增加协议/CMake复杂度。来源：[Protobuf `service.h` API](https://protobuf.dev/reference/cpp/api-docs/google.protobuf.service/)、[C++ generated code guide](https://protobuf.dev/reference/cpp/cpp-generated/)。

**Alternatives considered**:

- `RdmaConnection : RpcChannel` + 新 generic service proto：能学习调用链，但会新增 deprecated 生产接口和额外生成代码。
- 重写现有 gRPC codegen：拒绝，违反“不重写 RPC 框架”。
- 大 chunk 直接放 protobuf payload：拒绝，违背 control/data separation。

## Decision 3: Pull 与 Push 顺序实现，Push 第一版不做客户端 slot cache

**Decision**: 先实现 client MR + server RDMA READ；再实现 server 固定 slot pool + client RDMA WRITE。Push 每次通过控制消息向服务端申请 lease，不缓存 slot descriptor。

**Rationale**: Pull 最少状态，适合先验证 MR 生命周期和 durable ACK。Push 的 ownership 必须由服务端权威管理；跳过客户端缓存可以删除 refresh、失效同步和多客户端一致性逻辑，同时仍验证核心 WRITE 数据面。

**Alternatives considered**:

- 一开始长期缓存 slot map：拒绝，收益未测量且会引入 generation/refresh/reconciliation 状态机。
- 只做 Pull：不足以覆盖交接文档明确要求评估的 server slot 模式。
- 动态 MR/slot grow：拒绝，固定资源上界更容易验证。

## Decision 4: 只在远端数据操作明确未开始时 fallback

**Decision**: capability/resolve/connect/reserve-before-post 失败可 fallback；Pull 请求已被接受、Push WRITE 已 post 或 durable ACK 丢失时返回 uncertain，不盲目重发。

**Rationale**: RDMA completion 只代表网络操作完成，不代表 `ChunkStore` durable publish。没有额外远端状态查询时，after-post 失败无法安全证明远端没有数据或持久化动作。

**Alternatives considered**:

- 所有 timeout 都 fallback：拒绝，可能形成重复写并掩盖未知状态。
- 第一版新增 durable attempt 查询 RPC：拒绝，需要新的服务端持久化 attempt ledger，超出范围。
- 假设 chunk 幂等即可无条件重试：拒绝，虽然同内容写通常幂等，但不能用这一点隐藏 ACK/远端状态诊断。

## Decision 5: CMake 使用显式三态 capability mode

**Decision**: `CQUPT_RDMA_MODE=AUTO|ON|OFF`。AUTO 可生成 unavailable implementation；ON 缺依赖直接失败；OFF 明确禁用。

**Rationale**: Linux 开发环境未必有 RDMA headers/device，Windows/macOS 也不能编译 verbs。三态同时满足默认可构建、部署可强制要求、运行结果不伪装成功。

**Alternatives considered**:

- 无条件 required dependency：会破坏无 RDMA 环境的现有构建。
- 只用 bool option：无法区分“自动探测”和“用户强制要求”。
- 找不到依赖仍定义成功 stub：拒绝，属于 silent success。

## Decision 6: 最小验证集合

**Decision**: 一个 RDMA contract test target 覆盖 capability/connection/Pull/Push，一个 transfer integration target 覆盖 selection/fallback/uncertain；最终只加现有 upload 与 write contract 回归。

**Rationale**: 这是硬件相关新路径，需要验证数据丢失边界，但无需重复全仓 Raft/metadata 测试。真实硬件 smoke 与纯逻辑/fake transport 测试分开，避免无设备 CI 被误判成功或永久 skip。

**Alternatives considered**:

- 每个类单独单测：拒绝，文件和 fixture 数量过多。
- 只做真实硬件 E2E：拒绝，CI 不稳定且无法覆盖错误分类。
- 全仓 all group：拒绝，与本 feature 影响面不成比例。
