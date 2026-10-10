#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <google/protobuf/service.h>

namespace rdma
{
    class RdmaServer
    {
    public:
        explicit RdmaServer(google::protobuf::Service* service);
        ~RdmaServer();

        bool Start(std::string host, std::string service, std::string* error = nullptr);
        void Stop();
        std::string bound_address() const;

        void OnRpcRequest(const void* data, std::size_t length);
        void OnTransportFailure(const std::string& reason);
        std::size_t pending_request_count() const;

    protected:
        virtual bool SubmitFramedResponse(std::string framed_response, std::string* error);

    private:
        struct ServerRpcContext;
        class ServiceCompletionClosure;

        void CompleteRequest(std::shared_ptr<ServerRpcContext> context);
        void FinishRequest();
        void SendFailureResponse(std::uint64_t request_id, const std::string& reason);

        google::protobuf::Service* service_ = nullptr;
        mutable std::mutex mutex_;
        std::condition_variable idle_cv_;
        std::atomic<bool> stopped_{false};
        std::atomic<bool> transport_failed_{false};
        std::size_t in_flight_ = 0;
        std::string bound_address_;
    };
}
