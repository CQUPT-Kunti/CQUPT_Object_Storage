# store/common 说明

## 模块职责

`modules/store/common` 是 `storedemo` 数据面的基础类型模块。

这里放两类内容：

- 公共数据结构
- 不依赖 Raft、proto、KV 的基础 helper

这里不做：

- 本地文件落盘
- `ChunkStore` 业务实现
- StorageNode RPC
- ChunkIndex 容器实现

## 文件对照

- `store_types.h`：公开枚举、结构体、常量、helper 声明
- `store_types.cpp`：helper 具体实现

如果你想看“文档里的函数到底落在哪”，直接对这两个文件即可。

## 主要结构体

### `StorageNodeStatusCode`

store 侧统一错误码。

给后续 `WriteChunk`、`ReadChunk`、`Scrub`、`Repair` 共用，不等同于 proto 状态码，也不等同于 Raft 状态。

### `ChunkState`

表示 chunk 在本地节点上的状态。

- `kStaging`：正在写，不能对外读
- `kLive`：可读副本
- `kDeleting` / `kDeleted`：删除流程
- `kQuarantined` / `kCorrupted`：坏块或隔离块
- `kMissing`：预期存在但本地没找到

### `ChunkChecksum`

保存完整性校验结果。

关键字段：

- `algorithm`：当前算法
- `value`：摘要字符串
- `size_bytes`：参与计算的 payload 大小
- `computed_at`：预留时间字段

它只表示完整性校验，不表示内容寻址身份，也不做去重。

### `ChunkIdentity`

保存 chunk 的逻辑身份。

关键字段：

- `chunk_id`
- `object_id`
- `version`
- `chunk_index`
- `offset`

其中 `chunk_id` 基于 `object_id + version + chunk_index` 生成，`offset` 不参与生成。

### `ChunkLocation`

轻量位置引用，只表达“哪个 node 上的哪个 chunk”。

### `ChunkReplica`

表示副本的轻量事实，后续给副本选择、健康判断和 repair 复用。

### `ChunkMetadata`

表示本地节点看到的 chunk 元信息，不是全局 metadata source of truth。

特殊字段：

- `write_request_id`
- `delete_request_id`
- `quarantine_reason`

### `ChunkIndexEntry`

未来 `ChunkIndex` 单条记录的载体。

特殊字段：

- `final_path`
- `staging_path`
- `metadata_path`
- `lock_shard`

## `.cpp` 里当前实现了哪些函数

下面这些函数都在 `store_types.cpp` 里有对应实现：

### 状态和字符串 helper

- `ToString(StorageNodeStatusCode)`：错误码转稳定字符串
- `ToString(ChunkState)`：状态转稳定字符串
- `IsRetriableStatus(...)`：判断是否适合重试
- `IsReadableChunkState(...)`：判断状态是否可读
- `IsTerminalChunkState(...)`：判断状态是否终止

### chunk_id helper

规则是：

`object_id~version~chunk_index`

对应实现函数：

- `ValidateChunkObjectId(...)`：校验 `object_id`
- `MakeChunkId(...)`：生成 `chunk_id`
- `ParseChunkId(...)`：把 `chunk_id` 解析回 `ChunkIdentity`
- `ValidateChunkId(...)`：只校验，不返回解析结果

使用限制：

- `object_id` 不能为空
- 不能包含路径分隔符、`..` 或危险字符
- `version` 必须大于 0
- 总长度受控，便于后续本地路径布局

### checksum helper

对应实现函数：

- `ComputeChunkChecksum(...)`：对 payload 计算 checksum
- `VerifyChunkChecksum(...)`：拿 expected checksum 校验 payload

当前实现使用 SHA-256，小写十六进制输出。

### 结构体上的成员函数

这些成员函数也都在 `store_types.cpp` 里：

- `ChunkLocation::IsValid()`
- `ChunkChecksum::IsSet()`
- `ChunkIdentity::HasChunkKey()`
- `ChunkReplica::IsReadable()`
- `ChunkMetadata::IsReadable()`
- `ChunkIndexEntry::HasFinalPath()`

## 额外说明

- `store_types.cpp` 里还有 SHA-256 的内部匿名命名空间 helper，它们只服务 `ComputeChunkChecksum(...)`，不是对外 API。
- 如果这里新增结构体、字段或 helper，需要同时维护本说明，让文档名词能直接对上头文件和 `.cpp` 函数。

## SHA-256 在项目里的角色

这套 SHA-256 只承担完整性校验角色，不承担加密、保密、权限控制，也不是对象存储层的全局对象 ID 生成器。

更具体地说，它在当前项目里是一个 `chunk payload checksum`：

- 用来描述一段 payload 的内容摘要
- 用来校验写入前后的 payload 是否一致
- 用来帮助识别 chunk 是否损坏、截断、被错误覆盖
- 用来给 `read`、`scrub`、`repair` 等路径提供一致的校验依据

它不是：

- 可逆加密
- Raft 日志校验机制
- metadata record 主键
- 去重主键

### 它的作用是什么

#### 1. 写入路径上的准入校验

调用方如果在 `WriteChunk` 请求里带了 `expected_checksum`，底层会先对 `request.payload` 重新计算 SHA-256，再和请求里的期望值比对。

如果不一致，会直接返回 `kChecksumMismatch`，不会把错误 payload 当成成功写入。

这条链路的意义是：

- 防止上传端传错 payload
- 防止 chunk 尺寸和内容不匹配却继续落盘
- 让“写入成功”具备最基本的数据完整性前提

#### 2. 没有外部 checksum 时生成本地事实

如果请求没有显式给 `expected_checksum`，本地仍然会对 payload 计算 SHA-256，生成 `actual_checksum`，作为本地 chunk metadata 的一部分。

这让系统即使在“只给 payload，不给摘要”的路径下，也能得到统一的校验结果。

#### 3. 读取/巡检路径上的完整性确认

在 `ReadChunk`、`ScrubChunk`、恢复和本地文件检查等路径里，代码会重新读取 payload，再次计算 SHA-256，并与记录中的 checksum 对比。

如果内容和记录不一致，系统会把它视为损坏或不可信副本，而不是继续把它当成正常可读副本。

这个角色很关键，因为它直接决定：

- 某个副本是否还能参与读
- 某个 chunk 是否应该被隔离
- 某个副本是否需要 repair

### 它是怎么写的

当前实现没有引入 OpenSSL、Crypto++ 之类的外部依赖，而是在 `modules/store/common/store_types.cpp` 里手写了一版最小 SHA-256 实现。

实现入口和层次如下：

- 对外 API：`ComputeChunkChecksum(...)`
- 对外 API：`VerifyChunkChecksum(...)`
- 内部 helper：`ComputeSha256Checksum(...)`
- 内部过程：`UpdateSha256(...)`、`ProcessSha256Block(...)`、`FinalizeSha256(...)`

#### 1. 常量和状态

实现先定义了 SHA-256 需要的固定常量：

- 8 个初始 state word
- 64 个 round constants

然后用 `Sha256State` 保存运行时状态：

- `words`：当前 8 个工作字
- `buffer`：64 字节块缓存
- `buffer_size`：当前缓存占用
- `total_bytes`：总输入字节数

这就是一个标准的分块哈希状态机。

#### 2. 分块更新

`UpdateSha256(...)` 负责把任意长度的 payload 拆成 64 字节块推进状态机：

- 输入数据先拷进 `buffer`
- `buffer` 满 64 字节后调用 `ProcessSha256Block(...)`
- 持续推进直到所有输入都被消费

这意味着它支持：

- 空 payload
- 二进制 payload
- 非 64 字节对齐的数据

#### 3. 压缩函数

`ProcessSha256Block(...)` 是核心：

- 先把 64 字节 block 展开成 64 个 `schedule` word
- 前 16 个直接从输入块按 big-endian 读取
- 后 48 个按 SHA-256 标准递推生成
- 然后执行 64 轮压缩更新 `a..h`
- 最后把结果累加回 state

这部分就是标准 SHA-256 compression round，本项目没有改协议语义，也没有自定义变种。

#### 4. 收尾和输出

`FinalizeSha256(...)` 负责最后一步：

- 追加 `0x80`
- 补零到长度字段位置
- 在最后 8 字节写入总 bit 长度
- 再做最后一个或两个 block 的压缩
- 输出 32 字节 digest

随后 `EncodeLowerHex(...)` 会把 32 字节 digest 编码成 64 个小写十六进制字符。

所以当前项目里 `ChunkChecksum.value` 的表现形式是：

- 纯 64 位小写 hex 字符串
- 不带 `sha256:` 前缀

`ChunkChecksum.algorithm` 则单独保存算法类型 `kSha256`。

#### 5. 对外接口行为

`ComputeChunkChecksum(...)` 的行为很简单：

- 校验输出指针非空
- 直接调用内部 `ComputeSha256Checksum(payload)`
- 返回 `ChunkChecksum { algorithm=kSha256, value=<64 hex>, size_bytes=<payload.size()> }`

`VerifyChunkChecksum(...)` 则在校验前做几层前置检查：

- `expected_checksum.algorithm` 不能是 `kUnknown`
- 当前只支持 `kSha256`
- `expected_checksum.value` 长度必须是 64

然后它会重新计算实际摘要，并同时比较：

- `size_bytes`
- `value`

任一不一致都返回 `kChecksumMismatch`。

### 调用链上它处在什么位置

从职责分层看，SHA-256 的位置大致是：

- `store/node`：做 RPC proto 和本地结构之间的字段映射
- `store/common`：定义 checksum 类型和计算/校验 helper
- `store/chunk`：在真实 `WriteChunk` / `ReadChunk` / `Scrub` 流程里调用这些 helper

也就是说：

- `node` 层通常不负责真正算摘要
- 真正的 SHA-256 计算集中在 `store/common`
- 真正使用这些结果做业务判断的是 `chunk` 层

这套分层的目的很直接：把摘要算法本身做成公共基础能力，让存储路径复用同一份实现，而不是在 RPC、落盘、巡检各写一套。
