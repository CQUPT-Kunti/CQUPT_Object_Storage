#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <google/protobuf/service.h>

namespace rdma
{
    struct PendingRpc
    {
        std::uint64_t request_id = 0;
        google::protobuf::RpcController* controller = nullptr;
        google::protobuf::Message* response = nullptr;
        google::protobuf::Closure* done = nullptr;
    };

    class RdmaConnection : public google::protobuf::RpcChannel
    {
    public:
        RdmaConnection(std::string host, std::string service);
        ~RdmaConnection() override;

        void CallMethod(const google::protobuf::MethodDescriptor* method,
                        google::protobuf::RpcController* controller,
                        const google::protobuf::Message* request,
                        google::protobuf::Message* response,
                        google::protobuf::Closure* done) override;

        void Close();
        void OnTransportFailure(const std::string& reason);
        void OnRpcResponse(const void* data, std::size_t length);
        std::size_t pending_rpc_count() const;

    protected:
        virtual bool SubmitFramedRequest(std::string framed_request, std::string* error);

        bool RegisterPendingRpc(PendingRpc pending);
        std::optional<PendingRpc> TakePendingRpc(std::uint64_t request_id);
        std::vector<PendingRpc> DrainPendingRpcs();

    private:
        std::optional<std::uint64_t> NextRequestId();

        static void FailPendingRpc(const PendingRpc& pending, const std::string& reason);

        std::string host_;
        std::string service_;
        std::atomic<std::uint64_t> next_request_id_{1};
        std::atomic<bool> request_id_exhausted_{false};
        std::atomic<bool> closed_{false};
        mutable std::mutex pending_mutex_;
        std::unordered_map<std::uint64_t, PendingRpc> pending_rpcs_;
    };
}
