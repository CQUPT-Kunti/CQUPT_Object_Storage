#include "rdma/rdma_rpc_controller.h"

#include <utility>
#include <vector>

namespace rdma
{
    RdmaRpcController::RdmaRpcController() = default;

    RdmaRpcController::~RdmaRpcController() = default;

    void RdmaRpcController::Reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_ = false;
        error_text_.clear();
        canceled_ = false;
        cancel_callbacks_.clear();
    }

    bool RdmaRpcController::Failed() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return failed_;
    }

    std::string RdmaRpcController::ErrorText() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_text_;
    }

    void RdmaRpcController::SetFailed(const std::string& reason)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_ = true;
        error_text_ = reason;
    }

    void RdmaRpcController::StartCancel()
    {
        std::vector<google::protobuf::Closure*> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            canceled_ = true;
            callbacks.swap(cancel_callbacks_);
        }

        for (google::protobuf::Closure* callback : callbacks)
        {
            if (callback != nullptr)
            {
                callback->Run();
            }
        }
    }

    bool RdmaRpcController::IsCanceled() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return canceled_;
    }

    void RdmaRpcController::NotifyOnCancel(google::protobuf::Closure* callback)
    {
        if (callback == nullptr)
        {
            return;
        }

        bool run_now = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (canceled_)
            {
                run_now = true;
            }
            else
            {
                cancel_callbacks_.push_back(callback);
            }
        }

        if (run_now)
        {
            callback->Run();
        }
    }
}
