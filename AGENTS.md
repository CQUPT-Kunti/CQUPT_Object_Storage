# CQUPT_Raft

一句话说明：这是一个基于 C++20、gRPC、Protobuf、GoogleTest 的 Raft KV 内核项目，重点在一致性内核、持久化、快照、追赶和重启恢复。

## Language Rules

- 默认使用中文进行分析、规划、任务拆分、执行总结和测试结果说明。
- Spec Kit 生成的 spec.md、plan.md、tasks.md、validation-matrix.md 等自然语言文档优先使用中文。
- 代码标识符、文件名、目录名、类名、函数名、测试名、CMake target 名称保持英文。
- 命令行、CMake、bash、PowerShell、C++ 代码块保持原样。
- 技术术语允许中英混合，例如 Raft、leader election、AppendEntries、snapshot、follower catch-up、crash recovery、failure injection。

## 全局规则

- 每次聊天结束的最后都需要加上一个 "喵!" 来表示结尾
- 平台相关的 durability 代码不允许静默降级。
- 如果一个平台实现了真实持久化语义，其他平台分支必须提供等价行为、返回明确错误，或在 durability contract 中明确记录较弱保证。
- 对 required durability operations，不允许使用 no-op 后直接返回成功的实现。
- 按 AGENTS.md 执行。测试日志不要全文输出。通过只报 PASS；失败只报失败摘要、关键断言、失败分类、最后 50 行日志和完整日志文件路径。

## C++ 头文件 / 源文件规则

- `.h` 只写接口、类型、结构体、枚举、常量、必要注释和轻量 inline。
- `.cpp` 写具体实现、复杂逻辑、文件 IO、RPC 处理、持久化流程、平台相关代码和内部 helper。
- 默认优先改 `.cpp`；只有接口、类型、函数签名或契约变化时才改 `.h`。
- 修改 `.h` 必须说明原因和影响范围。
- 不要把复杂业务逻辑、Raft 状态转换、持久化 publish 流程、平台系统调用写进 `.h`。
- 内部 helper 优先放在 `.cpp` 的匿名 namespace 中。

## 使用规则

- 先读根 `AGENTS.md`。
- 再根据模块索引进入目标模块目录的 `AGENTS.md`。
- 再读取该模块源码与相关测试。
- 不要默认扫描整个仓库。

## 模块索引

- `modules/raft/common`
  - 共享配置、提案结果、命令编解码
- `modules/raft/metadata`
  - metadata-only 业务类型、命令 payload、查询模型
- `modules/raft/runtime`
  - 日志、定时器、线程池
- `modules/raft/service`
  - gRPC Raft/KV 服务适配层
- `modules/raft/node`
  - `RaftNode` 核心状态与调度
- `modules/raft/replication`
  - 单 follower 复制状态机
- `modules/raft/storage`
  - Raft 硬状态、segment log、snapshot catalog 持久化
- `modules/raft/state_machine`
  - KV / metadata 状态机与状态机快照格式
- `proto`
  - RPC/Protobuf 契约层
- `apps`
  - 节点入口与 CLI 客户端入口

## 全局硬规则

- 禁止读取 `.gitignore` 文件下面的文件夹里的任何文件。
- 禁止读取：
  - `vcpkg-configuration.json`
  - `CQUPT_Raft_AI_Context.md`
  - `README.md`
  - `/deploy`
- 不允许修改业务逻辑。
- 不允许修改协议语义。
- 不允许修改持久化格式。
- 不允许修改公共 API 行为。
- 不允许修改类名、函数名、命名空间。
- 不允许删除测试。
- 不允许跳过失败测试。
- 不允许为了通过编译而顺手改业务行为。

## 构建命令

```bash
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel
```

更保守：

```bash
cmake --preset debug-ninja-safe
cmake --build --preset debug-ninja-safe
```

## 测试命令

```bash
./test.sh
./test.sh --group unit
./test.sh --group persistence
./test.sh --group all
./test.sh --keep-data
```

建议保持低并发，例如：

```bash
CTEST_PARALLEL_LEVEL=1 ./test.sh --group all
```

## Test Log Output Rules
- 不要把完整测试日志粘贴到聊天里。
- 测试日志应保存到本地文件，例如 `tmp/test-logs/`。
- 测试通过时，只输出：
  - 测试命令
  - PASS
  - 总耗时
- 测试失败时，只输出：
  - 失败测试名
  - 关键断言
  - 失败分类
  - 最后 50 行日志
  - 本地完整日志文件路径
- 不要输出完整 Raft 节点日志，除非用户明确要求。
- 优先使用 `tail -n 50`、`grep -E`、`rg` 提取关键失败信息。
- 不要为了展示日志而重复运行测试。
```

## Include 规则

- 项目源码和测试统一从 `modules/` 作为 include 根目录。
- 头文件按模块引用：
  - `raft/common/...`
  - `raft/metadata/...`
  - `raft/runtime/...`
  - `raft/service/...`
  - `raft/node/...`
  - `raft/replication/...`
  - `raft/storage/...`
  - `raft/state_machine/...`
- 生成的 protobuf/gRPC 头文件保持直接引用：
  - `raft.pb.h`
  - `raft.grpc.pb.h`

## CMake 修改规则

- 优先保持现有 target 名称不变：
  - `raft_proto`
  - `raft_core`
  - `raft_demo`
  - `raft_kv_client`
- 只改路径、源文件列表、include 根目录和必要的构建脚本路径。
- 不要因为目录调整而引入新的业务逻辑分支。

## 跨模块修改规则

- 先定位主模块，再最小化波及范围。
- 如果修改 `modules/raft/node`，通常要检查：
  - `modules/raft/replication`
  - `modules/raft/storage`
  - `modules/raft/state_machine`
  - 相关 Raft 测试
- 如果修改 `proto` 或 `service`，必须同步检查契约使用方和测试。

## 高风险区域

- `proto/`
- `modules/raft/service`
- `modules/raft/storage`
- `modules/raft/node`
- `modules/raft/replication`
- snapshot/restart/catch-up 相关测试
- 并发与 crash recovery 路径

## 上下文节省规则

- 不要默认扫描整个仓库。
- 不要扫描NOTREAD.md文件里面标记的文件
- 先读根 `AGENTS.md`，再读目标模块 `AGENTS.md`。
- `.gitignore` 覆盖内容、构建产物、缓存、运行数据、日志、临时文件默认不读。
- 运行测试产生的 `raft_data/`、`raft_snapshots/`、`build/tests/raft_test_data/` 不要当作源码分析。
- 优先读：
  - `project_understanding.md`
  - `refactor_plan.md`
  - 根 `CMakeLists.txt`
  - `tests/CMakeLists.txt`
  - `proto/raft.proto`
  - 目标模块目录
  - 与目标模块直接相关的测试文件

<!-- SPECKIT START -->
For additional context about technologies to be used, project structure,
shell commands, and other important information, read
`specs/011-rdma-fast-path/plan.md`
<!-- SPECKIT END -->
