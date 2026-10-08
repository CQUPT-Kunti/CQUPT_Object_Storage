# Contract: RDMA Control And Data

## Wire Version

- 每条控制消息必须携带固定 magic 与 wire version `1`。
- 未知 version 必须在任何 RDMA data operation 前拒绝，因此允许安全 fallback。
- 控制消息有显式长度上限；不得携带 chunk payload。

## Operations

### Probe

1. Client 连接并发送 version/generation probe。
2. Server 返回 accepted version、current generation 和 capability。
3. 任一方关闭时释放连接资源。

### Pull Chunk

1. Client 构造现有 chunk 写元数据（payload 为空），注册 bounded payload MR。
2. Client 发送 metadata + remote read descriptor。
3. Server 校验 generation、identity、size 与 descriptor。
4. Server RDMA READ 到本地 bounded buffer，等待 read completion。
5. Server 校验 checksum，调用 `ChunkStore::WriteChunk()`。
6. Server 把现有 storage result 作为 durable response 返回。
7. Client 收到最终 response 后才释放 MR。

### Push Chunk

1. Client 请求与 request/chunk identity 绑定的 slot lease。
2. Server 原子地从 FREE 选择一个容量足够的 slot，返回 descriptor/generation。
3. Client RDMA WRITE 并等待本地 completion。
4. Client 发送 READY（不把 completion 当 durable success）。
5. Server 校验 owner/generation/length/checksum，调用 `ChunkStore::WriteChunk()`。
6. Server 返回 durable response，然后释放 slot。

## Invalid And Restart Behavior

- identity、size、checksum、generation 或 ownership 不匹配必须显式失败。
- listener 重启生成新 generation；旧 descriptor/lease 不迁移。
- control connection 丢失必须释放 RESERVED slot；WRITING/READY 的不确定结果不得由客户端自动 fallback。

## Deliberately Deferred

- generic `RpcChannel` service registry。
- client slot cache。
- multi-request pending map、batching、priority 和 shared CQ。
- multi-chunk parallel RDMA。
