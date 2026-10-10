#include "rdma/rdma_connection.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include "rdma/rdma_transport.h"

namespace rdma
{
    namespace
    {
        constexpr std::size_t kRequestFrameHeaderSize = 14u;
        constexpr std::size_t kResponseFrameHeaderSize = 18u;
        constexpr std::uint32_t kRpcStatusOk = 0u;

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

        std::string EncodeRequestFrame(const google::protobuf::MethodDescriptor& method,
                                       std::uint64_t request_id,
                                       const std::string& payload)
        {
            std::string frame;
            frame.reserve(kRequestFrameHeaderSize + payload.size());
            AppendLittleEndian(frame, static_cast<std::uint64_t>(method.index()), 2);
            AppendLittleEndian(frame, request_id, 8);
            AppendLittleEndian(frame, static_cast<std::uint64_t>(payload.size()), 4);
            frame.append(payload);
            return frame;
        }
    }

    RdmaConnection::RdmaConnection(std::string host, std::string service)
        : host_(std::move(host)),
          service_(std::move(service))
    {
    }

    RdmaConnection::~RdmaConnection()
    {
        Close();
    }

    bool RdmaConnection::Connect(std::string* error)
    {
        if (error != nullptr)
        {
            *error = "RDMA transport connect is not implemented yet (T005 transport interface pending)";
        }
        return false;
    }

    bool RdmaConnection::connected() const
    {
        return false;
    }

    std::size_t RdmaConnection::qp_count() const
    {
        return 0;
    }

    void RdmaConnection::CallMethod(const google::protobuf::MethodDescriptor* method,
                                    google::protobuf::RpcController* controller,
                                    const google::protobuf::Message* request,
                                    google::protobuf::Message* response,
                                    google::protobuf::Closure* done)
    {
        if (method == nullptr || request == nullptr || response == nullptr || done == nullptr)
        {
            if (controller != nullptr)
            {
                controller->SetFailed("RdmaConnection::CallMethod requires method, request, response, and done");
            }
            if (done != nullptr)
            {
                done->Run();
            }
            return;
        }

        if (closed_.load(std::memory_order_acquire))
        {
            if (controller != nullptr)
            {
                controller->SetFailed("RdmaConnection is closed");
            }
            done->Run();
            return;
        }

        std::string payload;
        if (!request->SerializeToString(&payload))
        {
            if (controller != nullptr)
            {
                controller->SetFailed("RdmaConnection: failed to serialize request for " + method->full_name());
            }
            done->Run();
            return;
        }

        const std::optional<std::uint64_t> request_id = NextRequestId();
        if (!request_id.has_value())
        {
            if (controller != nullptr)
            {
                controller->SetFailed("RdmaConnection: request id space is exhausted");
            }
            done->Run();
            return;
        }

        PendingRpc pending;
        pending.request_id = *request_id;
        pending.controller = controller;
        pending.response = response;
        pending.done = done;

        if (!RegisterPendingRpc(pending))
        {
            if (controller != nullptr)
            {
                controller->SetFailed("RdmaConnection is closed");
            }
            done->Run();
            return;
        }

        std::string framed_request = EncodeRequestFrame(*method, pending.request_id, payload);

        std::string error;
        if (!SubmitFramedRequest(std::move(framed_request), &error))
        {
            const std::optional<PendingRpc> failed = TakePendingRpc(pending.request_id);
            if (failed.has_value())
            {
                FailPendingRpc(*failed, error.empty() ? "RdmaConnection: request submission failed" : error);
            }
        }
    }

    void RdmaConnection::Close()
    {
        OnTransportFailure("RdmaConnection closed before the RPC completed");
    }

    void RdmaConnection::OnTransportFailure(const std::string& reason)
    {
        closed_.store(true, std::memory_order_release);

        const std::vector<PendingRpc> pending = DrainPendingRpcs();
        for (const PendingRpc& rpc : pending)
        {
            FailPendingRpc(rpc, reason);
        }
    }

    void RdmaConnection::OnRpcResponse(const void* data, std::size_t length)
    {
        if (data == nullptr || length < kResponseFrameHeaderSize)
        {
            return;
        }

        const auto* bytes = static_cast<const unsigned char*>(data);
        const std::uint64_t request_id = ReadLittleEndian(bytes + 2, 8);
        const std::uint64_t payload_length = ReadLittleEndian(bytes + 10, 4);
        const std::uint32_t status = static_cast<std::uint32_t>(ReadLittleEndian(bytes + 14, 4));

        if (payload_length != static_cast<std::uint64_t>(length - kResponseFrameHeaderSize) ||
            payload_length > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        {
            const std::optional<PendingRpc> malformed = TakePendingRpc(request_id);
            if (malformed.has_value())
            {
                FailPendingRpc(*malformed, "RdmaConnection: malformed RPC response frame");
            }
            return;
        }

        const std::optional<PendingRpc> pending = TakePendingRpc(request_id);
        if (!pending.has_value())
        {
            return;
        }

        if (status != kRpcStatusOk)
        {
            const std::string remote_error =
                payload_length > 0u
                    ? std::string(reinterpret_cast<const char*>(bytes + kResponseFrameHeaderSize),
                                  static_cast<std::size_t>(payload_length))
                    : std::string("RdmaConnection: remote RPC failed");
            FailPendingRpc(*pending, remote_error);
            return;
        }

        if (pending->response == nullptr ||
            !pending->response->ParseFromArray(bytes + kResponseFrameHeaderSize, static_cast<int>(payload_length)))
        {
            FailPendingRpc(*pending,
                           "RdmaConnection: failed to parse RPC response for request " + std::to_string(request_id));
            return;
        }

        if (pending->done != nullptr)
        {
            pending->done->Run();
        }
    }

    std::size_t RdmaConnection::pending_rpc_count() const
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        return pending_rpcs_.size();
    }

    bool RdmaConnection::SubmitFramedRequest(std::string framed_request, std::string* error)
    {
        (void)framed_request;

        const RdmaBackendInfo info = QueryRdmaBackendInfo();
        if (error != nullptr)
        {
            if (!info.available)
            {
                *error = "RDMA transport is unavailable: " + info.detail;
            }
            else
            {
                *error = "RDMA control transport send is not implemented yet (T005 transport interface pending)";
            }
        }
        return false;
    }

    bool RdmaConnection::RegisterPendingRpc(PendingRpc pending)
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (closed_.load(std::memory_order_acquire))
        {
            return false;
        }
        return pending_rpcs_.emplace(pending.request_id, pending).second;
    }

    std::optional<PendingRpc> RdmaConnection::TakePendingRpc(std::uint64_t request_id)
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        const auto entry = pending_rpcs_.find(request_id);
        if (entry == pending_rpcs_.end())
        {
            return std::nullopt;
        }

        const PendingRpc pending = entry->second;
        pending_rpcs_.erase(entry);
        return pending;
    }

    std::vector<PendingRpc> RdmaConnection::DrainPendingRpcs()
    {
        std::vector<PendingRpc> pending;
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending.reserve(pending_rpcs_.size());
        for (const auto& entry : pending_rpcs_)
        {
            pending.push_back(entry.second);
        }
        pending_rpcs_.clear();
        return pending;
    }

    std::optional<std::uint64_t> RdmaConnection::NextRequestId()
    {
        if (request_id_exhausted_.load(std::memory_order_acquire))
        {
            return std::nullopt;
        }

        const std::uint64_t request_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
        if (request_id == 0)
        {
            request_id_exhausted_.store(true, std::memory_order_release);
            return std::nullopt;
        }
        return request_id;
    }

    void RdmaConnection::FailPendingRpc(const PendingRpc& pending, const std::string& reason)
    {
        if (pending.controller != nullptr)
        {
            pending.controller->SetFailed(reason);
        }
        if (pending.done != nullptr)
        {
            pending.done->Run();
        }
    }
}
