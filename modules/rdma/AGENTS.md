# modules/rdma

一句话说明：`modules/rdma/` 是对象存储的 RDMA Fast Path 通用传输模块，只提供连接、QP、MR、SEND/RECV、READ/WRITE、completion 和 SlotPool 基础设施，不包含 Storage 业务语义。

## 模块定位

- RDMA 是原有 gRPC 对象存储路径之外的额外 Fast Path，默认优先 RDMA。
- RDMA 不可用或在远程数据操作开始前被明确拒绝时，才允许回退原有 gRPC；回退判定只允许 `not_started` 与 `rejected_before_transfer`，`remote_state_uncertain`、`failed`、`durable_success` 不允许回退。
- RDMA completion 只代表传输完成；唯一 durable 成功来自现有 `ChunkStore::WriteChunk()` 边界。
- 基础代码统一放在 `modules/rdma/`；本目录没有 module-local `CMakeLists.txt`，target（`rdma_proto`、`rdma_core`）由根 `CMakeLists.txt` 装配。

## 修改前必读

- 先读根 `AGENTS.md`，再读本文件，再读 `module-notes.md`。
- 设计依据：`specs/011-rdma-fast-path/spec.md`、`plan.md`、`research.md`、`data-model.md`、`contracts/`、`tasks.md`。
- 修改关键类型、状态机、generation 或生命周期规则时，必须同步更新 `module-notes.md`。
- 本目录不写任务执行流水账、调试记录或临时结论。

## 硬性规则

1. RDMA 是额外 Fast Path：默认优先 RDMA，安全失败时回退原有 gRPC；不允许把 fallback 用于 `remote_state_uncertain` / `failed` / `durable_success`。
2. RDMA 基础代码统一放 `modules/rdma/`；不允许放到 `modules/store/rdma/` 等业务目录。
3. Linux 必须使用 `librdmacm`（connect/listen/accept、CM 生命周期）+ `libibverbs`（PD/CQ/RC QP/MR/WR/completion）；其他平台走 portable unavailable backend，禁止静默降级。
4. `RdmaConnection` 继承 `google::protobuf::RpcChannel`，实现异步 RPC；`CallMethod()` 投递 SEND 后立即返回。
5. 一个 Client–StorageNode 对使用一个 `RdmaConnection`，MVP 只有一个 RC QP；控制 SEND/RECV、RDMA READ、RDMA WRITE、READY 共用同一 QP。
6. CQ 使用普通 `std::thread` completion（`ibv_comp_channel` + `ibv_get_cq_event` + 通知重注册 + `ibv_poll_cq` + event ack）；禁止 SPDK / FastBlock poller、共享 CQ 事件循环或通用 async runtime。
7. 使用 `request_id + PendingRpc` 关联异步请求与响应；`done` 在完成路径上必须恰好执行一次。
8. 临时内存使用 `RdmaMemoryRegion` 按需 `malloc → ibv_reg_mr → post WR → 完成路径 ibv_dereg_mr → free`；禁止通用 MemoryPool、buffer cache 或 MR 复用层。
9. RECV buffer 在连接期间保持注册并在处理完成后重投递；Push Slot 的 MR 在 descriptor 有效期内必须保持注册。二者是协议资源，不是通用内存池。
10. Store 为每个 Client 分配独立 SlotPool；Client 只缓存属于自己的 `RemoteSlotMap`（限本连接/本 generation），Store 始终是 pool 生命周期与 generation 的权威。
11. RDMA completion / READY 不等于 chunk 持久化成功；durable ACK 只能来自 `ChunkStore::WriteChunk()`。
12. `modules/rdma/` 不允许直接依赖 ChunkStore、Metadata、Raft、View 等业务实现；Storage 业务适配放在 `modules/store/node/storage_rdma_service.*`。
13. 不允许擅自增加共享 CQ 框架、request manager、task scheduler、优先级队列、timeout wheel、自动重连框架、operation registry、WR-id bit packing、多 service registry 等超出 Demo 范围的功能。
14. 不允许修改业务逻辑、协议语义、持久化格式或公共 API 行为；不允许删除或跳过测试。

## 编码要求

- `.h` 只放接口、类型、枚举和必要 inline；CM/verbs 调用、CQ 处理、异步 RPC 流程和生命周期管理放 `.cpp`。
- Linux 平台代码集中在 `rdma_transport_linux.cpp`；`rdma_transport.cpp` 只提供 portable unavailable 行为。
- include 以 `modules/` 为根，头文件按模块路径引用（如 `rdma/rdma_connection.h`）。
- 结构体、类、流程说明维护在 `module-notes.md`，不要散落大量代码注释。
