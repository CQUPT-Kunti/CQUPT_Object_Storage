#pragma once

#include <string>

#include <google/protobuf/service.h>

namespace rdma
{
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

    protected:
        virtual bool SubmitFramedRequest(std::string framed_request, std::string* error);

    private:
        std::string host_;
        std::string service_;
    };
}
