#include "rdma/rdma_connection.h"

#include <cstdint>
#include <string>
#include <utility>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include "rdma/rdma_transport.h"

namespace rdma
{
    namespace
    {
        constexpr std::uint64_t kUnassignedRequestId = 0u;
        constexpr std::size_t kFrameHeaderSize = 14u;

        void AppendLittleEndian(std::string& out, std::uint64_t value, int byte_count)
        {
            for (int index = 0; index < byte_count; ++index)
            {
                out.push_back(static_cast<char>((value >> (8 * index)) & 0xFFu));
            }
        }

        std::string EncodeRequestFrame(const google::protobuf::MethodDescriptor& method,
                                       std::uint64_t request_id,
                                       const std::string& payload)
        {
            std::string frame;
            frame.reserve(kFrameHeaderSize + payload.size());
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

    RdmaConnection::~RdmaConnection() = default;

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

        std::string framed_request = EncodeRequestFrame(*method, kUnassignedRequestId, payload);

        std::string error;
        if (!SubmitFramedRequest(std::move(framed_request), &error))
        {
            if (controller != nullptr)
            {
                controller->SetFailed(error.empty() ? "RdmaConnection: request submission failed" : error);
            }
            done->Run();
            return;
        }
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
}
