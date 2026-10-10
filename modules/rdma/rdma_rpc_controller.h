#pragma once

#include <mutex>
#include <string>
#include <vector>

#include <google/protobuf/service.h>

namespace rdma
{
    class RdmaRpcController : public google::protobuf::RpcController
    {
    public:
        RdmaRpcController();
        ~RdmaRpcController() override;

        void Reset() override;

        bool Failed() const override;
        std::string ErrorText() const override;
        void SetFailed(const std::string &reason) override;

        // 请求取消 RPC
        void StartCancel() override;
        // 查询是否收到取消请求
        bool IsCanceled() const override;
        // 注册取消通知回调
        void NotifyOnCancel(google::protobuf::Closure *callback) override;

    private:
        mutable std::mutex mutex_;
        bool failed_ = false;
        std::string error_text_;
        bool canceled_ = false;
        std::vector<google::protobuf::Closure *> cancel_callbacks_;
    };
}
