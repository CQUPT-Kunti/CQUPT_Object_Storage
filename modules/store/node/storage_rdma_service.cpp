#include "store/node/storage_rdma_service.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>

#include "store/common/store_types.h"
#include "store/runtime/storage_executor.h"

namespace storedemo
{
    namespace
    {
        // proto checksum → store 层 ChunkChecksum 的字段映射。
        ChunkChecksum ToStoreChecksum(const raft::rdma::RdmaChunkChecksum& checksum)
        {
            ChunkChecksum result;
            if (checksum.algorithm() == raft::rdma::RDMA_CHUNK_CHECKSUM_ALGORITHM_SHA256)
            {
                result.algorithm = ChunkChecksumAlgorithm::kSha256;
            }
            result.value = checksum.value();
            result.size_bytes = checksum.size_bytes();
            return result;
        }

        // 提交 READ 前的最小协议/资源边界校验，成功时输出解析好的 ChunkIdentity。
        bool ValidatePullRequest(const raft::rdma::PullRequest& request,
                                 ChunkIdentity* identity,
                                 std::string* error)
        {
            if (identity == nullptr || error == nullptr)
            {
                return false;
            }

            if (request.chunk_id().empty())
            {
                *error = "Pull requires a non-empty chunk_id";
                return false;
            }

            if (request.length() == 0)
            {
                *error = "Pull requires a non-zero length";
                return false;
            }

            if (request.length() > kProductionChunkSizeBytes)
            {
                *error = "Pull length exceeds the maximum supported chunk size";
                return false;
            }

            if (request.remote_address() == 0)
            {
                *error = "Pull requires a non-zero remote address";
                return false;
            }

            if (request.remote_rkey() == 0)
            {
                *error = "Pull requires a non-zero remote rkey";
                return false;
            }

            if (request.remote_address() > std::numeric_limits<std::uint64_t>::max() - request.length())
            {
                *error = "Pull remote address range overflows";
                return false;
            }

            std::string detail;
            if (ParseChunkId(request.chunk_id(), identity, &detail) != StorageNodeStatusCode::kOk)
            {
                *error = detail.empty() ? "Pull chunk_id is invalid" : detail;
                return false;
            }
            identity->offset = request.offset();

            const ChunkChecksum checksum = ToStoreChecksum(request.checksum());
            if (!checksum.IsSet())
            {
                *error = "Pull requires a supported checksum algorithm and value";
                return false;
            }
            return true;
        }

        // 可选地把 store 阶段提交到有界执行器；executor 为空时内联执行，避免阻塞 CQ 线程。
        bool RunOnExecutor(BoundedStorageExecutor* executor,
                           std::function<void()> task,
                           std::string* error)
        {
            if (executor == nullptr)
            {
                task();
                return true;
            }

            StorageExecutorSubmitRequest submit;
            submit.task_name = "rdma-pull";
            submit.task = std::move(task);
            const StorageExecutorSubmitResult result = executor->Submit(std::move(submit));
            if (!result.accepted())
            {
                *error = result.error_detail.empty() ? "storage executor rejected the RDMA pull task"
                                                     : result.error_detail;
                return false;
            }
            return true;
        }
    }

    // 跨异步阶段存活的一次 Pull 上下文；controller/response/done 由 RdmaServer 保证存活到 done。
    struct StorageRdmaService::PullContext
    {
        google::protobuf::RpcController* controller = nullptr;
        raft::rdma::PullResponse* response = nullptr;
        google::protobuf::Closure* done = nullptr;
        raft::rdma::PullRequest request;
        ChunkIdentity identity;
        std::unique_ptr<rdma::RdmaMemoryRegion> target_region;
        std::atomic<bool> finished{false};
    };

    // 组装 store、READ 后端与可选执行器。
    StorageRdmaService::StorageRdmaService(std::shared_ptr<ChunkStore> chunk_store,
                                           StorageRdmaReadBackend* read_backend,
                                           BoundedStorageExecutor* store_executor)
        : chunk_store_(std::move(chunk_store)),
          read_backend_(read_backend),
          store_executor_(store_executor)
    {
    }

    // Pull 入口：校验 → 建上下文 → 建目标 MR → 提交异步 READ → 返回；即时错误在此内联结束。
    void StorageRdmaService::Pull(google::protobuf::RpcController* controller,
                                  const raft::rdma::PullRequest* request,
                                  raft::rdma::PullResponse* response,
                                  google::protobuf::Closure* done)
    {
        if (request == nullptr || response == nullptr || done == nullptr)
        {
            if (controller != nullptr)
            {
                controller->SetFailed("StorageRdmaService::Pull requires request, response, and done");
            }
            if (done != nullptr)
            {
                done->Run();
            }
            return;
        }

        ChunkIdentity identity;
        std::string error;
        if (!ValidatePullRequest(*request, &identity, &error))
        {
            controller->SetFailed(error);
            done->Run();
            return;
        }

        if (chunk_store_ == nullptr)
        {
            controller->SetFailed("StorageRdmaService has no ChunkStore");
            done->Run();
            return;
        }

        if (read_backend_ == nullptr)
        {
            controller->SetFailed("RDMA Pull read backend is unavailable (T005 transport pending)");
            done->Run();
            return;
        }

        auto context = std::make_shared<PullContext>();
        context->controller = controller;
        context->response = response;
        context->done = done;
        context->request = *request;
        context->identity = identity;

        context->target_region =
            read_backend_->CreateTargetRegion(static_cast<std::size_t>(request->length()), &error);
        if (context->target_region == nullptr)
        {
            controller->SetFailed(error.empty() ? "failed to allocate the RDMA target region" : error);
            done->Run();
            return;
        }

        const bool submitted = read_backend_->SubmitRead(
            *context->target_region,
            *request,
            [this, context](bool ok, std::string read_error)
            { OnReadComplete(context, ok, std::move(read_error)); },
            &error);
        if (!submitted)
        {
            context->target_region.reset();
            controller->SetFailed(error.empty() ? "failed to submit the RDMA READ" : error);
            done->Run();
            return;
        }
    }

    // READ 完成回调：finished 原子标志保证重复 CQE 只处理一次；失败走 FailContext，成功进入 store 阶段。
    void StorageRdmaService::OnReadComplete(std::shared_ptr<PullContext> context, bool ok, std::string error)
    {
        if (context == nullptr || context->finished.exchange(true))
        {
            return;
        }

        if (!ok)
        {
            FailContext(context, error.empty() ? "RDMA READ failed" : error);
            return;
        }

        std::string executor_error;
        if (!RunOnExecutor(store_executor_,
                           [this, context] { RunStorePhase(context); },
                           &executor_error))
        {
            FailContext(context, executor_error);
        }
    }

    // durable 边界：只有 WriteChunk 返回成功才可能填 durable ACK；字节拷出后立即释放 MR。
    void StorageRdmaService::RunStorePhase(const std::shared_ptr<PullContext>& context)
    {
        std::string payload;
        if (context->target_region != nullptr && context->target_region->data() != nullptr)
        {
            payload.assign(static_cast<const char*>(context->target_region->data()),
                           static_cast<std::size_t>(context->request.length()));
        }
        context->target_region.reset();

        WriteChunkRequest write_request;
        write_request.request_id = "rdma-pull:" + context->request.chunk_id();
        write_request.identity = context->identity;
        write_request.expected_size = context->request.length();
        write_request.expected_checksum = ToStoreChecksum(context->request.checksum());
        write_request.payload = std::move(payload);

        const WriteChunkResponse write_response = chunk_store_->WriteChunk(write_request);

        if (context->response != nullptr)
        {
            context->response->set_chunk_id(context->request.chunk_id());
            context->response->set_ok(write_response.ok());
            context->response->set_already_exists(write_response.already_exists);
            context->response->set_durable(write_response.durable);
            context->response->set_error_detail(write_response.error_detail);
        }

        if (!write_response.ok() && context->controller != nullptr)
        {
            context->controller->SetFailed(write_response.error_detail.empty()
                                               ? "chunk store write failed"
                                               : write_response.error_detail);
        }

        if (context->done != nullptr)
        {
            context->done->Run();
        }
    }

    // 失败路径统一收尾，保证每个 RPC 只执行一次 done 并释放目标 MR。
    void StorageRdmaService::FailContext(const std::shared_ptr<PullContext>& context, const std::string& error)
    {
        if (context->controller != nullptr)
        {
            context->controller->SetFailed(error);
        }
        if (context->response != nullptr)
        {
            context->response->set_chunk_id(context->request.chunk_id());
            context->response->set_ok(false);
            context->response->set_durable(false);
            context->response->set_error_detail(error);
        }
        context->target_region.reset();

        if (context->done != nullptr)
        {
            context->done->Run();
        }
    }
}
