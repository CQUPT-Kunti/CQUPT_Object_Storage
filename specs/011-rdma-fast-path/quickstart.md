# Quickstart: RDMA Fast Path

本文件定义实现完成后的最小验证路径。命令按阶段执行，不要求每次运行全仓测试。

## 1. Configure

自动探测 RDMA：

```bash
cmake --preset debug-ninja-low-parallel -DCQUPT_RDMA_MODE=AUTO
```

具备 rdma-core 开发依赖、并要求缺失时立即失败：

```bash
cmake --preset debug-ninja-low-parallel -DCQUPT_RDMA_MODE=ON
```

明确关闭真实 RDMA、只验证 fallback：

```bash
cmake --preset debug-ninja-low-parallel -DCQUPT_RDMA_MODE=OFF
```

## 2. Stage-Level Validation

Stage 1-3 每完成一次，只运行 RDMA contract target：

```bash
cmake --build --preset debug-ninja-low-parallel --target test_rdma_transport_contract
ctest --test-dir build/debug-ninja-low-parallel -R '^rdma_transport_contract' --output-on-failure
```

Stage 4 完成后，只运行 transfer integration target：

```bash
cmake --build --preset debug-ninja-low-parallel --target test_storage_transfer_rdma
ctest --test-dir build/debug-ninja-low-parallel -R '^storage_transfer_rdma' --output-on-failure
```

## 3. Hardware Smoke

在 Linux RDMA 环境执行一次 64 MiB 单 chunk smoke：

1. 确认 configure 输出为真实 RDMA enabled，而不是 unavailable implementation。
2. 启动一个 StorageNode，确认 RDMA listener generation 已输出。
3. 上传一个 64 MiB 单 chunk 对象。
4. 确认诊断显示 RDMA durable success，且没有 fallback。
5. 读取对象并校验 checksum。
6. 重启 StorageNode，确认旧 generation descriptor/lease 被拒绝，再用新连接上传成功。

## 4. Final Related Regression

所有阶段完成后只执行一次相关集合：

```bash
cmake --build --preset debug-ninja-low-parallel
ctest --test-dir build/debug-ninja-low-parallel \
  -R '(rdma|storage_upload_integration|storage_write_chunk_contract)' \
  --output-on-failure
```

通过时只记录命令、PASS 与耗时；失败时保存完整日志，仅汇报失败测试、关键断言、分类、最后 50 行和日志路径。
