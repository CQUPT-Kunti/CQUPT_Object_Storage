# SHA-256 在项目中的角色分析

## 1. 先说结论

这个项目里的 SHA-256 不是“加密”，而是“摘要 / 完整性校验”。

它的核心职责是：

- 对 chunk payload 生成稳定摘要
- 在写入前校验请求里的 payload 是否正确
- 在读取、巡检、恢复时确认本地副本是否损坏
- 给 chunk 元信息提供统一的 checksum 事实

它不负责：

- 数据保密
- 权限控制
- Raft 日志签名
- 对象去重
- 作为对象主键或 chunk 主键

所以更准确的说法应该是：

这个项目使用 SHA-256 做数据完整性校验，而不是做数据加密。

## 2. SHA-256 在项目里扮演什么角色

从模块职责上看，SHA-256 处在 `store` 数据面内部，属于 chunk 内容校验基础设施。

它大致位于下面这条链路中：

- `modules/store/node`
  - 负责 RPC 请求 / 响应字段映射
  - 把 proto 里的 checksum 字段转成本地结构
- `modules/store/common`
  - 定义 checksum 类型
  - 实现 SHA-256 计算和校验 helper
- `modules/store/chunk`
  - 在真实的写入、读取、scrub、repair 路径里调用这些 helper

也就是说：

- `node` 层不是真正计算 SHA-256 的地方
- `common` 层是算法和通用 helper 的实现位置
- `chunk` 层才是把 SHA-256 结果用于业务判断的地方

## 3. 它的主要作用是什么

### 3.1 写入时校验 payload

写 chunk 时，如果请求里带了 `expected_checksum`，系统会先对 `payload` 重新计算 SHA-256，再和请求中的期望值比较。

如果不一致：

- 返回 `kChecksumMismatch`
- 不把这份 payload 当作成功写入

这个动作的意义是：

- 防止客户端传错内容
- 防止内容被截断或篡改后仍继续落盘
- 让写入成功建立在“内容匹配”的前提上

### 3.2 写入时生成本地 checksum 事实

如果请求没有显式提供 `expected_checksum`，系统也会自己对 payload 计算 SHA-256。

这样做的作用是：

- 给本地 chunk metadata 留下一份统一摘要
- 让后续读、巡检、恢复路径都能复用同一种校验依据

### 3.3 读取时确认副本完整性

读 chunk 时，系统可以把读出的 payload 再计算一次 SHA-256，并和记录值比较。

如果不一致，说明这个副本虽然“文件还在”，但“内容已经不可信”。

这种情况下它不应该继续被当作正常副本使用，而应该进入损坏、隔离或 repair 相关路径。

### 3.4 巡检和修复时识别坏块

在 scrub、恢复和本地一致性检查路径里，SHA-256 的作用更直接：

- 判断磁盘上的 chunk 文件是否和记录一致
- 发现 silent corruption
- 发现文件被截断或内容错乱
- 为后续 repair 提供“这个副本是否可信”的依据

## 4. 它是怎么写的

当前实现没有依赖 OpenSSL、Crypto++ 或其他外部密码库，而是在项目内部手写了一版最小可用的 SHA-256 实现。

实现文件在：

- `modules/store/common/store_types.cpp`

对外暴露的主要接口在：

- `modules/store/common/store_types.h`

### 4.1 对外接口

项目主要通过两个函数使用 SHA-256：

- `ComputeChunkChecksum(...)`
- `VerifyChunkChecksum(...)`

这两个函数的职责很清楚：

- `ComputeChunkChecksum(...)`
  - 输入 payload
  - 输出 `ChunkChecksum`
- `VerifyChunkChecksum(...)`
  - 输入 payload 和 expected checksum
  - 重新计算实际 checksum
  - 比较是否一致

对应的数据结构是 `ChunkChecksum`，其中保存：

- `algorithm`
- `value`
- `size_bytes`
- `computed_at`

其中当前真正生效的算法是：

- `ChunkChecksumAlgorithm::kSha256`

## 5. 内部实现分几步

### 5.1 维护 SHA-256 常量

在 `store_types.cpp` 内部，先定义了：

- 8 个初始状态常量
- 64 个轮常量

这是标准 SHA-256 所需的固定参数。

### 5.2 使用状态机累积输入

实现里定义了一个内部 `Sha256State`，用来维护运行时状态：

- 8 个工作字
- 一个 64 字节缓冲区
- 当前缓冲区大小
- 总输入字节数

这说明实现采用的是标准的“分块处理”思路，而不是一次性只处理固定长度数据。

### 5.3 按 64 字节 block 处理数据

`UpdateSha256(...)` 会持续把输入 payload 喂给状态机：

- 不够 64 字节时先放进缓冲区
- 凑满 64 字节后就处理一个 block
- 直到全部输入消费完

这样它天然支持：

- 空字符串
- 普通文本
- 二进制 payload
- 非 64 字节对齐的数据

### 5.4 做 64 轮压缩

`ProcessSha256Block(...)` 是核心计算函数。

它做的事是：

- 把当前 64 字节 block 解释成前 16 个 word
- 递推出完整的 64 个 schedule word
- 执行 64 轮 SHA-256 压缩
- 把结果累加回 state

这部分就是标准 SHA-256 的 compression round。

### 5.5 收尾并输出十六进制摘要

`FinalizeSha256(...)` 负责最后的收尾：

- 追加 `0x80`
- 补零
- 写入总 bit 长度
- 完成最后一次 block 处理
- 输出 32 字节 digest

之后 `EncodeLowerHex(...)` 把 digest 编码成小写十六进制字符串。

所以项目里实际保存到 `ChunkChecksum.value` 的是：

- 64 个小写十六进制字符

不是二进制原始 digest，也不是带前缀的格式串。

## 6. 校验逻辑是怎么工作的

`VerifyChunkChecksum(...)` 的逻辑很直接：

1. 先检查 `expected_checksum.algorithm` 是否已设置
2. 当前只接受 `kSha256`
3. 再检查 `expected_checksum.value` 长度是否为 64
4. 重新对 payload 计算实际摘要
5. 比较：
   - `size_bytes`
   - `value`
6. 不一致就返回 `kChecksumMismatch`

也就是说，项目不是只比摘要字符串，还会比 payload 大小。

## 7. 代码里的实际落点

### 7.1 计算与校验实现

实现主要在：

- `modules/store/common/store_types.cpp`

关键函数包括：

- `ComputeChunkChecksum(...)`
- `VerifyChunkChecksum(...)`
- `ComputeSha256Checksum(...)`
- `UpdateSha256(...)`
- `ProcessSha256Block(...)`
- `FinalizeSha256(...)`
- `EncodeLowerHex(...)`

### 7.2 写入路径使用点

实际写入 chunk 时，`LocalDiskChunkStore::WriteChunk(...)` 会：

- 如果请求带了 expected checksum，就先校验
- 否则直接计算实际 checksum

所以 SHA-256 是写路径上的准入条件之一。

### 7.3 读/巡检路径使用点

在本地 chunk 内容读取和一致性检查路径里，也会重新计算 checksum，再和已有记录比较。

如果不一致，副本会被视为损坏或不可信，而不是继续正常参与读路径。

## 8. 为什么项目要把它放在 `store/common`

这个放置是合理的，因为 SHA-256 既不是：

- RPC 协议适配逻辑
- 落盘实现细节
- Raft 控制面逻辑

它本质上是一个“所有存储路径共用的基础校验能力”。

放在 `store/common` 的好处是：

- `node`、`chunk`、`maintenance` 可以共用一套实现
- 避免不同路径各自实现一份 checksum 逻辑
- 让错误码、checksum 类型和 helper 保持一致

## 9. 总结

在这个项目里，SHA-256 的角色可以概括成一句话：

它是 `store` 数据面里用于 chunk payload 完整性校验的统一摘要机制。

它的核心作用不是“保护别人看不见数据”，而是“保证系统读到和写入的是同一份正确数据”。

实现方式上，项目没有依赖外部密码库，而是在 `modules/store/common/store_types.cpp` 里手写了一版标准 SHA-256，并通过 `ComputeChunkChecksum(...)` 与 `VerifyChunkChecksum(...)` 向上层提供统一接口。
