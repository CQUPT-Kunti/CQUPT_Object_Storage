#include "rdma/rdma_server.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include "rdma/rdma_rpc_controller.h"

namespace rdma
{
    namespace
    {
        constexpr std::size_t kRequestFrameHeaderSize = 14u;
        constexpr std::size_t kResponseFrameHeaderSize = 18u;
        constexpr std::uint32_t kRpcStatusOk = 0u;
        constexpr std::uint32_t kRpcStatusFailed = 1u;

        void AppendLittleEndian(std::string& out, std::uint64_t value, int byte_count)
        {
            for (int index = 0; index < byte_count; ++index)
            {
                out.push_back(static_cast<char>((value >> (8 * index)) & 0xFFu));
            }
        }

        std::uint64_t ReadLittleEndian(const unsigned char* data, int byte_count)
        {
            std::uint64_t value = 0;
            for (int index = 0; index < byte_count; ++index)
            {
                value |= static_cast<std::uint64_t>(data[index]) << (8 * index);
            }
            return value;
        }

        std::string EncodeResponseFrame(std::uint64_t request_id,
                                        std::uint32_t status,
                                        const std::string& payload)
        {
            std::string frame;
            frame.reserve(kResponseFrameHeaderSize + payload.size());
            AppendLittleEndian(frame, 0, 2);
            AppendLittleEndian(frame, request_id, 8);
            AppendLittleEndian(frame, static_cast<std::uint64_t>(payload.size()), 4);
            AppendLittleEndian(frame, static_cast<std::uint64_t>(status), 4);
            frame.append(payload);
            return frame;
        }
    }

    struct RdmaServer::ServerRpcContext
    {
        std::uint64_t request_id = 0;
        std::unique_ptr<google::protobuf::Message> request;
        std::unique_ptr<google::protobuf::Message> response;
        RdmaRpcController controller;
    };

    class RdmaServer::ServiceCompletionClosure final : public google::protobuf::Closure
    {
    public:
        ServiceCompletionClosure(RdmaServer* server, std::shared_ptr<ServerRpcContext> context)
            : server_(server),
              context_(std::move(context))
        {
        }

        void Run() override
        {
            server_->CompleteRequest(context_);
            delete this;
        }

    private:
        RdmaServer* server_;
        std::shared_ptr<ServerRpcContext> context_;
    };

    RdmaServer::RdmaServer(google::protobuf::Service* service)
        : service_(service)
    {
    }

    RdmaServer::~RdmaServer()
    {
        Stop();
    }

    bool RdmaServer::Start(std::string host, std::string service, std::string* error)
    {
        (void)host;
        (void)service;

        if (service_ == nullptr)
        {
            if (error != nullptr)
            {
                *error = "RdmaServer requires a non-null service";
            }
            return false;
        }

        if (stopped_.load(std::memory_order_acquire))
        {
            if (error != nullptr)
            {
                *error = "RdmaServer is already stopped";
            }
            return false;
        }

        if (error != nullptr)
        {
            *error = "RDMA server listen is not implemented yet (T005 transport interface pending)";
        }
        return false;
    }

    void RdmaServer::Stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_.store(true, std::memory_order_release);
        }

        std::unique_lock<std::mutex> lock(mutex_);
        idle_cv_.wait(lock, [this] { return in_flight_ == 0; });
    }

    std::string RdmaServer::bound_address() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return bound_address_;
    }

    void RdmaServer::OnTransportFailure(const std::string& reason)
    {
        (void)reason;

        transport_failed_.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_.store(true, std::memory_order_release);
    }

    std::size_t RdmaServer::pending_request_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return in_flight_;
    }

    void RdmaServer::OnRpcRequest(const void* data, std::size_t length)
    {
        if (stopped_.load(std::memory_order_acquire) || service_ == nullptr)
        {
            return;
        }

        if (data == nullptr || length < kRequestFrameHeaderSize)
        {
            return;
        }

        const auto* bytes = static_cast<const unsigned char*>(data);
        const std::uint64_t method_index = ReadLittleEndian(bytes, 2);
        const std::uint64_t request_id = ReadLittleEndian(bytes + 2, 8);
        const std::uint64_t payload_length = ReadLittleEndian(bytes + 10, 4);

        if (payload_length != static_cast<std::uint64_t>(length - kRequestFrameHeaderSize) ||
            payload_length > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        {
            SendFailureResponse(request_id, "RdmaServer: malformed RPC request frame");
            return;
        }

        const google::protobuf::ServiceDescriptor* service_descriptor = service_->GetDescriptor();
        const google::protobuf::MethodDescriptor* method = nullptr;
        if (service_descriptor != nullptr &&
            method_index < static_cast<std::uint64_t>(service_descriptor->method_count()))
        {
            method = service_descriptor->method(static_cast<int>(method_index));
        }

        if (method == nullptr)
        {
            SendFailureResponse(request_id, "RdmaServer: unknown RPC method index " + std::to_string(method_index));
            return;
        }

        auto context = std::make_shared<ServerRpcContext>();
        context->request_id = request_id;
        context->request.reset(service_->GetRequestPrototype(method).New());
        context->response.reset(service_->GetResponsePrototype(method).New());

        if (context->request == nullptr || context->response == nullptr)
        {
            SendFailureResponse(request_id, "RdmaServer: failed to allocate request/response prototypes");
            return;
        }

        if (!context->request->ParseFromArray(bytes + kRequestFrameHeaderSize, static_cast<int>(payload_length)))
        {
            SendFailureResponse(request_id, "RdmaServer: failed to parse RPC request");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_.load(std::memory_order_acquire))
            {
                return;
            }
            ++in_flight_;
        }

        auto* done = new ServiceCompletionClosure(this, context);
        service_->CallMethod(method, &context->controller, context->request.get(), context->response.get(), done);
    }

    bool RdmaServer::SubmitFramedResponse(std::string framed_response, std::string* error)
    {
        (void)framed_response;

        if (error != nullptr)
        {
            *error = "RDMA server transport send is not implemented yet (T005 transport interface pending)";
        }
        return false;
    }

    void RdmaServer::CompleteRequest(std::shared_ptr<ServerRpcContext> context)
    {
        if (!stopped_.load(std::memory_order_acquire) && !transport_failed_.load(std::memory_order_acquire))
        {
            std::string payload;
            std::uint32_t status = kRpcStatusOk;

            if (context->controller.Failed())
            {
                status = kRpcStatusFailed;
                payload = context->controller.ErrorText();
            }
            else if (!context->response->SerializeToString(&payload))
            {
                status = kRpcStatusFailed;
                payload = "RdmaServer: failed to serialize RPC response";
            }

            std::string frame = EncodeResponseFrame(context->request_id, status, payload);
            std::string error;
            SubmitFramedResponse(std::move(frame), &error);
        }

        FinishRequest();
    }

    void RdmaServer::FinishRequest()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (in_flight_ > 0)
            {
                --in_flight_;
            }
        }
        idle_cv_.notify_all();
    }

    void RdmaServer::SendFailureResponse(std::uint64_t request_id, const std::string& reason)
    {
        if (stopped_.load(std::memory_order_acquire) || transport_failed_.load(std::memory_order_acquire))
        {
            return;
        }

        std::string frame = EncodeResponseFrame(request_id, kRpcStatusFailed, reason);
        std::string error;
        SubmitFramedResponse(std::move(frame), &error);
    }
}
