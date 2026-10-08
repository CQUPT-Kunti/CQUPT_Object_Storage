# GC 任务池代码分析

> 对应源码：`modules/store/maintenance/garbage_collector.h` / `.cpp`
> 说明：把 GC 任务池的全部关键代码提取出来，只看骨架和核心逻辑，便于理解整体设计。

---

## 一、枚举与类型定义（`garbage_collector.h`）

### 1.1 GC 触发原因

```cpp
enum class GarbageCollectionReason : std::uint8_t
{
    kUnspecified = 0,
    kDeletedObjectCleanup = 1,   // 对象已删除，清理其 chunk
    kOrphanChunkCleanup = 2,     // 孤立 chunk（如 PENDING 超时）
    kFailedUploadCleanup = 3,    // 上传失败，清理已写入的 chunk
    kAbortCleanup = 4,           // 主动中止，清理 chunk
};
```

### 1.2 任务状态机

```cpp
enum class GarbageCollectorTaskState : std::uint8_t
{
    kQueued = 0,       // 已入队，等待 worker 消费
    kRunning = 1,      // worker 正在执行
    kRetryPending = 2, // 执行失败，等待重试
    kCompleted = 3,    // 执行成功
    kFailed = 4,       // 最终失败（超过最大重试次数）
    kCancelled = 5,    // 被取消（如 Stop(kCancelPending) 时丢弃）
};
```

状态转换图：

```
SubmitTask()
    ↓
kQueued ──worker 调度──→ kRunning
                            ↓
                     safety_checker → delete_handler
                            ↓
                    ┌── ok ──→ kCompleted
                    │
                    └── 失败且可重试 ──→ kRetryPending ──→ (重试) ──→ kRunning
                    │
                    └── 失败且不可重试 ──→ kFailed
                    
Stop(kCancelPending)
    ↓
kQueued ──→ kCancelled
```

### 1.3 提交结果码

```cpp
enum class GarbageCollectorSubmitCode : std::uint8_t
{
    kAccepted = 0,        // 提交成功，任务已入队
    kOverloaded = 1,      // 队列满了，拒绝
    kStopped = 2,         // GC 已停止，不再接收新任务
    kInvalidArgument = 3, // 参数非法
    kAlreadyExists = 4,   // 相同 task_id 已存在
};
```

### 1.4 停机模式

```cpp
enum class GarbageCollectorStopMode : std::uint8_t
{
    kDrain = 0,          // 优雅停止：等所有已提交任务执行完
    kCancelPending = 1,  // 强制停止：丢弃还在排队的任务
};
```

### 1.5 配置

```cpp
struct GarbageCollectorConfig
{
    std::size_t worker_count{1};         // worker 线程数 (= 底层 BoundedStorageExecutor 的 worker 数)
    std::size_t queue_capacity{64};      // 底层执行器的队列容量
    std::uint32_t default_max_attempts{3}; // 默认最大重试次数
    std::filesystem::path persistence_root; // 持久化根目录（空=不持久化）
};
```

### 1.6 任务结构体

```cpp
struct GarbageCollectorTask
{
    std::string task_id;                          // 唯一标识
    ChunkId chunk_id;                             // 要清理的 chunk
    std::string object_id;                        // 所属对象
    std::uint64_t version{0};
    std::uint32_t chunk_index{0};
    GarbageCollectionReason reason;               // 清理原因
    std::string metadata_boundary;                // metadata 安全边界描述
    std::uint32_t attempts{0};                    // 已尝试次数
    std::uint32_t max_attempts{0};                // 最大尝试次数
    StorageNodeStatusCode last_error;             // 上次错误码
    std::string last_error_detail;                // 上次错误详情
    GarbageCollectorTaskState state{kQueued};     // 当前状态
    bool retryable{false};                        // 是否可重试
    std::uint64_t next_retry_after_ms{0};         // 建议重试等待时间
};
```

### 1.7 CleanupCandidate — 外部传入的"清理候选"

```cpp
struct CleanupCandidate
{
    CleanupCandidateSource source;         // 来源（删除/PENDING超时/上传失败/中止）
    CleanupObjectState object_state;       // 对象状态
    GarbageCollectionReason reason;
    std::string bucket;
    std::string object_key;
    ChunkIdentity identity;
    std::uint64_t size{0};
    ChunkChecksum checksum;
    std::vector<StorageNodeId> replica_nodes;
    std::string metadata_boundary;
    std::uint64_t created_at_unix_ms{0};
    std::uint64_t deadline_unix_ms{0};
};
```

### 1.8 统计快照

```cpp
struct GarbageCollectorStats
{
    bool accepting_new_tasks;       // 是否接收新任务
    bool stop_requested;            // 是否已请求停机
    std::size_t worker_count;       // worker 数量
    std::size_t queue_capacity;     // 队列容量
    std::size_t queued_tasks;       // 排队中的任务数
    std::size_t running_tasks;      // 运行中的任务数
    std::size_t retry_pending_tasks;// 等待重试的任务数
    std::size_t completed_tasks;    // 已完成任务数
    std::size_t failed_tasks;       // 失败任务数
    std::size_t cancelled_tasks;    // 已取消任务数
    std::uint64_t submitted_tasks;  // 累计提交数
    std::uint64_t rejected_tasks;   // 累计拒绝数
    std::uint64_t total_attempts;   // 累计执行尝试次数
    std::string last_error_detail;  // 最近错误详情
};
```

---

## 二、GarbageCollector 类骨架（`garbage_collector.h`）

```cpp
class GarbageCollector
{
public:
    // 构造函数：需要传入 delete_handler 和 safety_checker 两个回调
    explicit GarbageCollector(
        GarbageCollectorDeleteHandler delete_handler,      // 实际执行删除的回调
        GarbageCollectorSafetyChecker safety_checker,      // 删除前的安全检测回调
        GarbageCollectorConfig config = {});

    ~GarbageCollector();   // 析构时自动 Stop(kDrain)

    // 提交一个 GC 任务
    GarbageCollectorSubmitResult SubmitTask(GarbageCollectorTask task);

    // 批量提交 cleanup candidates（自动转成 GC task）
    GarbageCollectorCleanupHookResult SubmitCleanupCandidates(
        const std::vector<CleanupCandidate> &candidates);

    // 等待所有任务结束（阻塞）
    GarbageCollectorDrainResult Drain();

    // 停止 GC（Drain 或 CancelPending）
    GarbageCollectorStopResult Stop(GarbageCollectorStopRequest request = {});

    // 查找任务
    std::optional<GarbageCollectorTask> FindTask(std::string_view task_id) const;

    // 获取统计快照
    GarbageCollectorStats SnapshotStats() const;

    const GarbageCollectorConfig &config() const;

private:
    struct Impl;        // 所有实现细节隐藏在 Impl 中
    std::unique_ptr<Impl> impl_;
    GarbageCollectorConfig config_;
};
```

两个回调的类型：

```cpp
// 删除回调：给定一个 GC task，执行实际的 chunk 删除
using GarbageCollectorDeleteHandler =
    std::function<DeleteChunkResponse(const GarbageCollectorTask &)>;

// 安全检测回调：删除前检查 metadata，确认可以安全删除
using GarbageCollectorSafetyChecker =
    std::function<GarbageCollectorSafetyCheckResult(const GarbageCollectorTask &)>;
```

---

## 三、Impl 内部结构（`garbage_collector.cpp`）

### 3.1 内部数据结构

```cpp
struct GarbageCollector::Impl
{
    // 构造时创建底层 BoundedStorageExecutor
    explicit Impl(GarbageCollectorDeleteHandler handler,
                  GarbageCollectorSafetyChecker checker,
                  const GarbageCollectorConfig &collector_config)
        : delete_handler(std::move(handler))
        , safety_checker(std::move(checker))
        , config(collector_config)
        , executor(StorageExecutorConfig{                           // ← 底层执行器
              .worker_count = collector_config.worker_count,        //     默认 1 个 worker
              .queue_capacity = collector_config.queue_capacity})   //     默认队列容量 64
        , task_store(collector_config.persistence_root.empty()
                         ? nullptr
                         : std::make_shared<GarbageCollectorTaskStore>(...))
    {
    }

    mutable std::mutex mutex;                              // 保护 tasks 和状态
    std::condition_variable cv;

    GarbageCollectorDeleteHandler delete_handler;          // 外部注入的删除回调
    GarbageCollectorSafetyChecker safety_checker;          // 外部注入的安全检测回调
    GarbageCollectorConfig config;

    BoundedStorageExecutor executor;                       // ← 任务真正排队和执行的地方
    std::shared_ptr<GarbageCollectorTaskStore> task_store; // 持久化存储（可选）

    bool accepting_new_tasks{true};
    bool stop_requested{false};
    GarbageCollectorStopMode stop_mode{GarbageCollectorStopMode::kDrain};

    std::uint64_t submitted_tasks{0};
    std::uint64_t rejected_tasks{0};
    std::uint64_t total_attempts{0};
    std::string last_error_detail;

    // 所有任务（含已完成/已失败的）都保存在这里
    std::unordered_map<std::string, GarbageCollectorTask> tasks;
    //         ↑ task_id  →  task 对象
};
```

**要点**：

```
GarbageCollector 有两层存储：
  第一层：tasks (unordered_map)
    └── 保存所有任务的全生命周期状态（含已完成）
    └── 用于 FindTask、SnapshotStats、Drain 检测
    
  第二层：executor (BoundedStorageExecutor) 内部的 deque
    └── 只保存"等待执行"的任务
    └── 每次 ScheduleTaskExecution 提交一个 lambda 到 executor
```

---

## 四、构造函数（启动流程）

```cpp
GarbageCollector::GarbageCollector(
    GarbageCollectorDeleteHandler delete_handler,
    GarbageCollectorSafetyChecker safety_checker,
    GarbageCollectorConfig config)
    : impl_(std::make_unique<Impl>(...))
    , config_(SanitizeGarbageCollectorConfig(config))
{
    // 1. 检查两个回调不为空
    if (!impl_->delete_handler)   throw ...;
    if (!impl_->safety_checker)   throw ...;

    // 2. 如果启用了持久化，从磁盘加载已有任务
    if (impl_->PersistenceEnabled())
    {
        const auto load_result = impl_->task_store->LoadSnapshot();
        if (load_result.ok() && load_result.snapshot_found)
        {
            // 对每个加载的任务：
            //   a. 验证有效性
            //   b. 恢复状态（kRunning → kQueued）
            //   c. 如果是可恢复状态（Queued/RetryPending），
            //      调用 ScheduleTaskExecution 重新提交到 executor
            for (const auto &loaded_task : load_result.tasks) {
                ValidateRecoveredTask(&task);
                NormalizeRecoveredTaskState(&task);
                tasks.emplace(task.task_id, task);
                if (IsRecoverableTaskState(task.state)) {
                    ScheduleTaskExecution(task.task_id);
                }
            }
        }
    }
}
```

---

## 五、核心：SubmitTask 完整流程

```cpp
GarbageCollectorSubmitResult GarbageCollector::SubmitTask(GarbageCollectorTask task)
{
    // 第 1 步：校验 task 合法性
    //   - task_id 不能为空
    //   - metadata_boundary 不能为空
    //   - reason 不能是 Unspecified
    //   - attempts < max_attempts
    //   - chunk_id 必须合法
    auto status = ValidateTaskForSubmission(&task, config_, &validation_error);
    if (status != kOk) return {.code = kInvalidArgument, .error_detail = ...};

    // 第 2 步：加锁，检查状态，写入 tasks 和持久化
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        
        // 检查 GC 是否还在接收新任务
        if (!impl_->accepting_new_tasks)
            return {.code = kStopped, ...};
        
        // 检查 task_id 是否已存在（幂等保护）
        if (impl_->tasks.find(task.task_id) != impl_->tasks.end())
            return {.code = kAlreadyExists, ...};
        
        // 先写入 tasks（持久化保护：持久化失败就回滚）
        impl_->tasks.emplace(task.task_id, task);
        auto persist_status = impl_->PersistTasksLocked(&persist_error);
        if (persist_status != kOk) {
            impl_->tasks.erase(task.task_id);  // 回滚
            return {.code = kStopped, ...};
        }
    }

    // 第 3 步：把任务调度到 BoundedStorageExecutor 执行
    const auto executor_result = impl_->ScheduleTaskExecution(task_id);

    // 第 4 步：如果 executor 拒绝（队列满），回滚 tasks
    if (!executor_result.accepted()) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->tasks.erase(task_id);
        (void)impl_->PersistTasksLocked(nullptr);
        ++impl_->rejected_tasks;
        return {.code = kOverloaded, ...};
    }

    // 第 5 步：提交成功
    ++impl_->submitted_tasks;
    return {.code = kAccepted, ...};
}
```

---

## 六、核心：ScheduleTaskExecution（任务执行逻辑）

```cpp
StorageExecutorSubmitResult GarbageCollector::Impl::ScheduleTaskExecution(
    const std::string &task_id)
{
    // 向 BoundedStorageExecutor 提交一个 lambda
    return executor.Submit(StorageExecutorSubmitRequest{
        .task_name = "garbage-collector/" + task_id,
        .task =
            [this, task_id]()
            {
                // ====== 这个 lambda 在 executor 的 worker 线程中执行 ======
                for (;;)   // 内层循环：支持重试
                {
                    // === Phase 1: 状态检查 → kRunning ===
                    GarbageCollectorTask task_snapshot;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        
                        // 如果任务被取消了，直接退出
                        if (tasks[task_id].state == kCancelled) return;
                        
                        // 标记为 Running
                        tasks[task_id].state = GarbageCollectorTaskState::kRunning;
                        PersistTasksLocked(nullptr);   // 持久化状态变更
                        task_snapshot = tasks[task_id];
                    }

                    // === Phase 2: 安全检测 ===
                    GarbageCollectorAttemptResult attempt_result;
                    try {
                        // 先问 safety_checker："这个 chunk 可以删吗？"
                        attempt_result = MakeAttemptResultFromSafetyCheck(
                            safety_checker(task_snapshot));
                    } catch (...) { attempt_result.status = kIoError; }

                    // === Phase 3: 实际删除（安全检测通过后）===
                    if (attempt_result.status == StorageNodeStatusCode::kOk)
                    {
                        try {
                            // 执行实际的删除操作
                            attempt_result = MakeAttemptResultFromDeleteResponse(
                                delete_handler(task_snapshot));
                        } catch (...) { attempt_result.status = kIoError; }
                    }

                    // === Phase 4: 更新状态 ===
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        ++tasks[task_id].attempts;
                        ++total_attempts;

                        if (attempt_result.status == kOk) {
                            // 成功 → kCompleted
                            tasks[task_id].state = GarbageCollectorTaskState::kCompleted;
                            should_retry = false;
                        } else {
                            // 失败 → 判断是否可重试
                            bool can_retry =
                                IsRetriableStatus(attempt_result.status)  // 错误可重试？
                                && tasks[task_id].attempts < tasks[task_id].max_attempts  // 没超上限？
                                && !(stop_requested && stop_mode == kCancelPending);  // 没在强制停机？

                            if (can_retry) {
                                tasks[task_id].state = GarbageCollectorTaskState::kRetryPending;
                                should_retry = true;  // ← 内层循环继续
                            } else {
                                tasks[task_id].state = GarbageCollectorTaskState::kFailed;
                                should_retry = false;  // ← 内层循环退出
                            }
                        }
                        PersistTasksLocked(nullptr);
                    }

                    if (!should_retry) return;  // 退出 lambda
                    // 如果 should_retry，继续内层 for 循环
                }
            }});
}
```

**要点**：
- `ScheduleTaskExecution` 只向 executor 提交一次 lambda
- 这个 lambda 内部用 `for(;;)` 循环处理重试
- 重试是**立即重试**，没有退避延迟
- 一旦达到 `max_attempts` 或遇到不可重试错误，lambda 退出

---

## 七、完整的执行流程时序

```
调用方（如 UploadCoordinator / 外部 GC 触发者）
    │
    │  SubmitTask(task)
    ▼
┌───────────────────────────────────────────────┐
│ GarbageCollector::SubmitTask                   │
│                                                │
│  ① ValidateTaskForSubmission                   │
│  ② 加锁 → tasks.emplace(task_id, task)         │
│  ③ PersistTasksLocked (持久化到磁盘)            │
│  ④ ScheduleTaskExecution(task_id)              │
│     └── executor.Submit(lambda)                │
│         ├── 队列没满 → 入队，返回 Accepted      │
│         └── 队列满了 → 回滚 tasks.erase, 返回   │
│                       Overloaded               │
└───────────────────────────────────────────────┘
    │
    ▼  (executor worker 线程取到任务)
┌───────────────────────────────────────────────┐
│ lambda (worker thread)                         │
│                                                │
│  for(;;) {                                     │
│    task.state = kRunning                       │
│    safety_checker(task)  → "可以删吗？"         │
│    delete_handler(task)   → "执行删除"          │
│                                                │
│    if (成功)   → state = kCompleted, break     │
│    if (可重试) → state = kRetryPending, continue│
│    if (不可重试)→ state = kFailed, break       │
│  }                                             │
└───────────────────────────────────────────────┘
```

---

## 八、批量提交：SubmitCleanupCandidates

```cpp
GarbageCollectorCleanupHookResult GarbageCollector::SubmitCleanupCandidates(
    const std::vector<CleanupCandidate> &candidates)
{
    // 对每个 candidate：
    for (const auto &candidate : candidates) {
        // 1. 转成 GC task
        const auto task = CleanupCandidateToGarbageCollectorTask(candidate);
        //    task_id = "gc-candidate/DeletedObject/obj~v1~0"

        // 2. 提交
        const auto submit_result = SubmitTask(task);

        // 3. 统计
        if (accepted)       ++submitted_count;
        if (already_exists) ++already_exists_count;
        if (rejected)       ++rejected_count;
    }

    return result;
}
```

---

## 九、Stop / Drain

```cpp
GarbageCollectorStopResult GarbageCollector::Stop(GarbageCollectorStopRequest request)
{
    // 1. 设置停止标志
    impl_->accepting_new_tasks = false;
    impl_->stop_requested = true;

    // 2. 如果是 CancelPending 模式，把 kQueued 任务标记为 kCancelled
    if (request.mode == kCancelPending) {
        for (auto &[task_id, task] : impl_->tasks) {
            if (task.state == kQueued) {
                task.state = kCancelled;
            }
        }
    }

    // 3. 关闭底层 executor
    impl_->executor.Shutdown({
        .mode = TranslateStopMode(request.mode)
    });

    // 4. 返回统计
    return result;
}

GarbageCollectorDrainResult GarbageCollector::Drain()
{
    // 阻塞直到所有任务都到达终态（Completed/Failed/Cancelled）
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->cv.wait(lock, [this]() {
        return !HasPendingWork(impl_->tasks);
    });
    return {.drained = true};
}
```

---

## 十、整体架构图

```
┌────────────────────────────────────────────────────────────────┐
│                     GarbageCollector                            │
│                                                                │
│  ┌──────────────────────────────────────────────┐              │
│  │  tasks (unordered_map<string, GCTask>)        │              │
│  │  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐        │              │
│  │  │task_A│ │task_B│ │task_C│ │task_D│  ...    │              │
│  │  │Queued│ │Running│ │Compl │ │Failed│        │              │
│  │  └──────┘ └──────┘ └──────┘ └──────┘        │              │
│  └──────────────────────────────────────────────┘              │
│                                                                │
│  ┌──────────────────────────────────────────────┐              │
│  │  BoundedStorageExecutor                       │              │
│  │                                               │              │
│  │   queue (deque<QueuedTask>)                   │              │
│  │   ┌──────────┐                                │              │
│  │   │ lambda_A │  ← 执行 task_A 的 lambda       │              │
│  │   └──────────┘                                │              │
│  │                                               │              │
│  │   worker threads:                             │              │
│  │   ┌────────┐                                  │              │
│  │   │ worker │──→ 取 lambda → 执行 → 循环重试    │              │
│  │   └────────┘                                  │              │
│  └──────────────────────────────────────────────┘              │
│                                                                │
│  ┌──────────────────────────────────────────────┐              │
│  │  GarbageCollectorTaskStore（可选持久化）       │              │
│  │                                               │              │
│  │  ┌─────────────────────────────────┐          │              │
│  │  │  gc/tasks.snapshot (文件)       │          │              │
│  │  │  重启时恢复未完成的任务           │          │              │
│  │  └─────────────────────────────────┘          │              │
│  └──────────────────────────────────────────────┘              │
└──────────────────────────────────────────────────────────────┘
```

---

## 十一、关键设计要点

| 要点 | 说明 |
|------|------|
| **两层存储** | `tasks` (unordered_map) 存全部任务 + `executor.queue` (deque) 只存等待执行的任务 |
| **幂等保护** | `task_id` 唯一，重复提交返回 `kAlreadyExists` |
| **安全检测** | 执行删除前先调 `safety_checker` 问 metadata 层"能不能删" |
| **持久化恢复** | 重启时自动从磁盘恢复未完成的任务（`kRunning` → `kQueued` 降级后重新执行） |
| **有界队列** | executor 队列满时返回 overloaded，不会无限积压 |
| **有界重试** | `max_attempts`（默认 3），超过后标记为 `kFailed` |
| **无权决定** | GC 只负责删除编排，`safety_checker` 决定了能不能删，这是 metadata 层的 authority |

喵！
