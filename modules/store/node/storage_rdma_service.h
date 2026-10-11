#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include <google/protobuf/service.h>

#include "rdma.pb.h"
#include "rdma/rdma_memory_region.h"
#include "store/chunk/chunk_store.h"

namespace storedemo
{
    class BoundedStorageExecutor;

    // Pull 的 RDMA READ 执行后端 seam；真实实现由 T013/T005 接线，测试注入替身。
    class StorageRdmaReadBackend
    {
    public:
        // READ 完成回调：ok=false 时 error 说明原因；实现必须恰好调用一次。
        using ReadCompletion = std::function<void(bool ok, std::string error)>;

        StorageRdmaReadBackend() = default;
        StorageRdmaReadBackend(const StorageRdmaReadBackend&) = delete;
        StorageRdmaReadBackend& operator=(const StorageRdmaReadBackend&) = delete;
        virtual ~StorageRdmaReadBackend() = default;

        // 分配并注册本次 Pull 的服务端目标 MR（IBV_ACCESS_LOCAL_WRITE），失败返回 nullptr + error。
        virtual std::unique_ptr<rdma::RdmaMemoryRegion> CreateTargetRegion(std::size_t size,
                                                                           std::string* error) = 0;
        // 用请求中的远端 addr/rkey/length 在同一 RC QP 上提交异步 RDMA READ。
        virtual bool SubmitRead(rdma::RdmaMemoryRegion& target,
                                const raft::rdma::PullRequest& request,
                                ReadCompletion completion,
                                std::string* error) = 0;
    };

    // StorageNode 侧 Pull RPC 适配器：RDMA READ 取数据，持久化走现有 ChunkStore。
    class StorageRdmaService final : public raft::rdma::RdmaControlService
    {
    public:
        // chunk_store 提供持久化语义；read_backend 执行 READ；store_executor 可选，用于把 store 阶段移出 CQ 线程。
        StorageRdmaService(std::shared_ptr<ChunkStore> chunk_store,
                           StorageRdmaReadBackend* read_backend,
                           BoundedStorageExecutor* store_executor = nullptr);

        // Pull RPC 入口：校验后提交异步 READ 并立即返回，后续由 READ 完成回调驱动。
        void Pull(google::protobuf::RpcController* controller,
                  const raft::rdma::PullRequest* request,
                  raft::rdma::PullResponse* response,
                  google::protobuf::Closure* done) override;

    private:
        struct PullContext;

        // READ 完成回调：恰好一次地分流到失败收尾或 store 阶段。
        void OnReadComplete(std::shared_ptr<PullContext> context, bool ok, std::string error);
        // store 阶段：拷出字节、释放 MR、调用 WriteChunk、填响应并执行 done。
        void RunStorePhase(const std::shared_ptr<PullContext>& context);
        // 统一失败收尾：SetFailed、响应置失败、释放 MR、执行 done。
        void FailContext(const std::shared_ptr<PullContext>& context, const std::string& error);

        std::shared_ptr<ChunkStore> chunk_store_;
        StorageRdmaReadBackend* read_backend_ = nullptr;
        BoundedStorageExecutor* store_executor_ = nullptr;
    };
}
