#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "rdma/rdma_transport.h"

#include "rdma.pb.h"

#if defined(CQUPT_RDMA_ENABLED) && CQUPT_RDMA_ENABLED
#if defined(__has_include)
#if __has_include("rdma/rdma_connection.h") && __has_include("rdma/rdma_server.h")
#include "rdma/rdma_connection.h"
#include "rdma/rdma_server.h"
#define CQUPT_RDMA_CONTRACT_HAS_ASYNC_IMPL 1
#endif
#endif
#endif

#ifndef CQUPT_RDMA_CONTRACT_HAS_ASYNC_IMPL
#define CQUPT_RDMA_CONTRACT_HAS_ASYNC_IMPL 0
#endif

namespace
{
    constexpr auto kAsyncTimeout = std::chrono::seconds(5);
    constexpr auto kSettleDelay = std::chrono::milliseconds(100);

    class CountingClosure final : public google::protobuf::Closure
    {
    public:
        void Run() override
        {
            runs.fetch_add(1, std::memory_order_acq_rel);
        }

        std::atomic<int> runs{0};
    };

    class WaitingClosure final : public google::protobuf::Closure
    {
    public:
        void Run() override
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++runs_;
            }
            cv_.notify_all();
        }

        bool WaitForRuns(int expected, std::chrono::milliseconds timeout)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            return cv_.wait_for(lock, timeout, [&] { return runs_ >= expected; });
        }

        int runs() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return runs_;
        }

    private:
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        int runs_ = 0;
    };

    class TestRpcController final : public google::protobuf::RpcController
    {
    public:
        void Reset() override
        {
            failed_ = false;
            canceled_ = false;
            error_text_.clear();
        }

        bool Failed() const override
        {
            return failed_;
        }

        std::string ErrorText() const override
        {
            return error_text_;
        }

        void StartCancel() override
        {
            canceled_ = true;
        }

        void SetFailed(const std::string& reason) override
        {
            failed_ = true;
            error_text_ = reason;
        }

        bool IsCanceled() const override
        {
            return canceled_;
        }

        void NotifyOnCancel(google::protobuf::Closure*) override
        {
        }

    private:
        bool failed_ = false;
        bool canceled_ = false;
        std::string error_text_;
    };

    class FakeRpcChannel final : public google::protobuf::RpcChannel
    {
    public:
        void CallMethod(const google::protobuf::MethodDescriptor* method,
                        google::protobuf::RpcController* controller,
                        const google::protobuf::Message* request,
                        google::protobuf::Message* response,
                        google::protobuf::Closure* done) override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            method_ = method;
            controller_ = controller;
            request_ = request;
            response_ = response;
            done_ = done;
            ++call_count_;
        }

        void CompleteProbeResponse(uint64_t probe_id, const std::string& message)
        {
            google::protobuf::Closure* done = nullptr;
            raft::rdma::ProbeResponse* response = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                done = done_;
                response = static_cast<raft::rdma::ProbeResponse*>(response_);
            }

            response->set_probe_id(probe_id);
            response->set_ok(true);
            response->set_message(message);
            if (done != nullptr)
            {
                done->Run();
            }
        }

        void FailPendingCall(const std::string& reason)
        {
            google::protobuf::RpcController* controller = nullptr;
            google::protobuf::Closure* done = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                controller = controller_;
                done = done_;
            }

            if (controller != nullptr)
            {
                controller->SetFailed(reason);
            }
            if (done != nullptr)
            {
                done->Run();
            }
        }

        const google::protobuf::MethodDescriptor* method() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return method_;
        }

        const google::protobuf::Message* request() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return request_;
        }

        int call_count() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return call_count_;
        }

    private:
        mutable std::mutex mutex_;
        const google::protobuf::MethodDescriptor* method_ = nullptr;
        google::protobuf::RpcController* controller_ = nullptr;
        const google::protobuf::Message* request_ = nullptr;
        google::protobuf::Message* response_ = nullptr;
        google::protobuf::Closure* done_ = nullptr;
        int call_count_ = 0;
    };

    class RecordingProbeService final : public raft::rdma::RdmaControlService
    {
    public:
        void Probe(google::protobuf::RpcController*,
                   const raft::rdma::ProbeRequest* request,
                   raft::rdma::ProbeResponse* response,
                   google::protobuf::Closure* done) override
        {
            ++calls_;
            response->set_probe_id(request->probe_id());
            response->set_ok(true);
            if (done != nullptr)
            {
                done->Run();
            }
        }

        int calls() const
        {
            return calls_;
        }

    private:
        int calls_ = 0;
    };

    TEST(RdmaTransportContractTest, BackendSelectionMatchesCompiledConfiguration)
    {
        const rdma::RdmaBackendInfo info = rdma::QueryRdmaBackendInfo();

#if defined(CQUPT_RDMA_ENABLED) && CQUPT_RDMA_ENABLED
        EXPECT_TRUE(info.available);
        EXPECT_EQ(info.kind, rdma::RdmaBackendKind::kLinuxVerbs);
        EXPECT_EQ(info.backend_name, "librdmacm+libibverbs");
        EXPECT_FALSE(info.detail.empty());
#else
        EXPECT_FALSE(info.available);
        EXPECT_EQ(info.kind, rdma::RdmaBackendKind::kUnavailable);
        EXPECT_EQ(info.backend_name, "unavailable");
        EXPECT_FALSE(info.detail.empty());
#endif
    }

    TEST(RdmaTransportContractTest, UnavailableBackendReportsExplicitReason)
    {
#if defined(CQUPT_RDMA_ENABLED) && CQUPT_RDMA_ENABLED
        GTEST_SKIP() << "RDMA backend is enabled in this build; unavailable path is not active";
#else
        const rdma::RdmaBackendInfo info = rdma::QueryRdmaBackendInfo();
        EXPECT_FALSE(info.available);
        EXPECT_EQ(info.backend_name, "unavailable");
        EXPECT_FALSE(info.detail.empty());
#endif
    }

    TEST(RdmaTransportContractTest, BackendInfoIsStableAcrossQueries)
    {
        const rdma::RdmaBackendInfo first = rdma::QueryRdmaBackendInfo();
        const rdma::RdmaBackendInfo second = rdma::QueryRdmaBackendInfo();

        EXPECT_EQ(first.available, second.available);
        EXPECT_EQ(first.kind, second.kind);
        EXPECT_EQ(first.backend_name, second.backend_name);
        EXPECT_EQ(first.detail, second.detail);
    }

    TEST(RdmaTransportContractTest, GeneratedStubSubmitsProbeThroughRpcChannelBeforeCallback)
    {
        FakeRpcChannel channel;
        raft::rdma::RdmaControlService::Stub stub(&channel);
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(7);
        raft::rdma::ProbeResponse response;
        CountingClosure done;

        stub.Probe(&controller, &request, &response, &done);

        ASSERT_NE(channel.method(), nullptr);
        EXPECT_EQ(channel.method()->full_name(), "raft.rdma.RdmaControlService.Probe");
        EXPECT_EQ(channel.call_count(), 1);
        EXPECT_EQ(done.runs.load(), 0);

        const auto* captured_request = dynamic_cast<const raft::rdma::ProbeRequest*>(channel.request());
        ASSERT_NE(captured_request, nullptr);
        EXPECT_EQ(captured_request->probe_id(), 7u);

        channel.CompleteProbeResponse(7, "probe-ok");

        EXPECT_EQ(done.runs.load(), 1);
        EXPECT_FALSE(controller.Failed());
        EXPECT_EQ(response.probe_id(), 7u);
        EXPECT_TRUE(response.ok());
        EXPECT_EQ(response.message(), "probe-ok");
    }

    TEST(RdmaTransportContractTest, GeneratedStubPropagatesChannelFailureThroughController)
    {
        FakeRpcChannel channel;
        raft::rdma::RdmaControlService::Stub stub(&channel);
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(8);
        raft::rdma::ProbeResponse response;
        CountingClosure done;

        stub.Probe(&controller, &request, &response, &done);
        EXPECT_EQ(done.runs.load(), 0);

        channel.FailPendingCall("transport failure");

        EXPECT_EQ(done.runs.load(), 1);
        EXPECT_TRUE(controller.Failed());
        EXPECT_EQ(controller.ErrorText(), "transport failure");
    }

    TEST(RdmaTransportContractTest, ProbeMessagesRoundTripThroughSerialization)
    {
        raft::rdma::ProbeRequest request;
        request.set_probe_id(4242);

        std::string request_bytes;
        ASSERT_TRUE(request.SerializeToString(&request_bytes));

        raft::rdma::ProbeRequest parsed_request;
        ASSERT_TRUE(parsed_request.ParseFromString(request_bytes));
        EXPECT_EQ(parsed_request.probe_id(), 4242u);

        raft::rdma::ProbeResponse response;
        response.set_probe_id(4242);
        response.set_ok(true);
        response.set_message("round-trip");

        std::string response_bytes;
        ASSERT_TRUE(response.SerializeToString(&response_bytes));

        raft::rdma::ProbeResponse parsed_response;
        ASSERT_TRUE(parsed_response.ParseFromString(response_bytes));
        EXPECT_EQ(parsed_response.probe_id(), 4242u);
        EXPECT_TRUE(parsed_response.ok());
        EXPECT_EQ(parsed_response.message(), "round-trip");
    }

    TEST(RdmaTransportContractTest, TruncatedControlPayloadFailsToParse)
    {
        raft::rdma::ProbeRequest request;
        request.set_probe_id(1);

        std::string bytes;
        ASSERT_TRUE(request.SerializeToString(&bytes));
        ASSERT_FALSE(bytes.empty());

        raft::rdma::ProbeRequest parsed;
        EXPECT_FALSE(parsed.ParseFromArray(bytes.data(), 1));

        const char malformed_length[] = {0x12, 0x7f, 0x01};
        raft::rdma::ProbeRequest parsed_length;
        EXPECT_FALSE(parsed_length.ParseFromArray(malformed_length, sizeof(malformed_length)));
    }

    TEST(RdmaTransportContractTest, RdmaControlServiceDescriptorExposesSingleProbeMethod)
    {
        const google::protobuf::ServiceDescriptor* descriptor = raft::rdma::RdmaControlService::descriptor();

        ASSERT_NE(descriptor, nullptr);
        EXPECT_EQ(descriptor->name(), "RdmaControlService");
        EXPECT_EQ(descriptor->method_count(), 1);
        EXPECT_EQ(descriptor->method(0)->name(), "Probe");
        EXPECT_EQ(descriptor->method(0)->input_type()->name(), "ProbeRequest");
        EXPECT_EQ(descriptor->method(0)->output_type()->name(), "ProbeResponse");
        EXPECT_EQ(descriptor->FindMethodByName("Missing"), nullptr);
    }

    TEST(RdmaTransportContractTest, GeneratedServiceDispatchInvokesProbeAndRunsClosureOnce)
    {
        RecordingProbeService service;
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(9);
        raft::rdma::ProbeResponse response;
        CountingClosure done;

        service.CallMethod(raft::rdma::RdmaControlService::descriptor()->method(0),
                           &controller,
                           &request,
                           &response,
                           &done);

        EXPECT_EQ(service.calls(), 1);
        EXPECT_EQ(done.runs.load(), 1);
        EXPECT_EQ(response.probe_id(), 9u);
        EXPECT_FALSE(controller.Failed());
    }

#if CQUPT_RDMA_CONTRACT_HAS_ASYNC_IMPL

    bool HasRdmaDevice()
    {
#ifdef __linux__
        std::error_code error;
        const std::filesystem::path devices("/sys/class/infiniband");
        return std::filesystem::exists(devices, error) && !std::filesystem::is_empty(devices, error);
#else
        return false;
#endif
    }

    class DeferredProbeService final : public raft::rdma::RdmaControlService
    {
    public:
        void Probe(google::protobuf::RpcController* controller,
                   const raft::rdma::ProbeRequest* request,
                   raft::rdma::ProbeResponse* response,
                   google::protobuf::Closure* done) override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            contexts_.push_back(Context{request->probe_id(), controller, response, done, false});
            cv_.notify_all();
        }

        bool WaitForPending(std::size_t count, std::chrono::milliseconds timeout)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            return cv_.wait_for(lock, timeout, [&]
                                { return static_cast<std::size_t>(std::count_if(
                                             contexts_.begin(), contexts_.end(),
                                             [](const Context& context) { return !context.completed; })) >= count; });
        }

        bool Complete(uint64_t probe_id, const std::string& message)
        {
            Context context;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = std::find_if(contexts_.begin(), contexts_.end(),
                                       [&](const Context& candidate)
                                       { return candidate.probe_id == probe_id && !candidate.completed; });
                if (it == contexts_.end())
                {
                    return false;
                }
                it->completed = true;
                context = *it;
            }

            context.response->set_probe_id(probe_id);
            context.response->set_ok(true);
            context.response->set_message(message);
            if (context.done != nullptr)
            {
                context.done->Run();
            }
            return true;
        }

        bool ReplayCompletedClosure(uint64_t probe_id)
        {
            Context context;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = std::find_if(contexts_.begin(), contexts_.end(),
                                       [&](const Context& candidate)
                                       { return candidate.probe_id == probe_id && candidate.completed; });
                if (it == contexts_.end())
                {
                    return false;
                }
                context = *it;
            }

            if (context.done != nullptr)
            {
                context.done->Run();
            }
            return true;
        }

        bool Fail(uint64_t probe_id, const std::string& reason)
        {
            Context context;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = std::find_if(contexts_.begin(), contexts_.end(),
                                       [&](const Context& candidate)
                                       { return candidate.probe_id == probe_id && !candidate.completed; });
                if (it == contexts_.end())
                {
                    return false;
                }
                it->completed = true;
                context = *it;
            }

            if (context.controller != nullptr)
            {
                context.controller->SetFailed(reason);
            }
            if (context.done != nullptr)
            {
                context.done->Run();
            }
            return true;
        }

    private:
        struct Context
        {
            uint64_t probe_id;
            google::protobuf::RpcController* controller;
            raft::rdma::ProbeResponse* response;
            google::protobuf::Closure* done;
            bool completed;
        };

        std::mutex mutex_;
        std::condition_variable cv_;
        std::vector<Context> contexts_;
    };

    class LoopbackPair
    {
    public:
        bool Start(google::protobuf::Service* service, std::string* error)
        {
            server_ = std::make_unique<rdma::RdmaServer>(service);
            if (!server_->Start("", "0", error))
            {
                return false;
            }

            const std::string address = server_->bound_address();
            const std::size_t separator = address.rfind(':');
            if (separator == std::string::npos)
            {
                if (error != nullptr)
                {
                    *error = "bound address is not host:service";
                }
                return false;
            }

            connection_ = std::make_unique<rdma::RdmaConnection>(address.substr(0, separator),
                                                                 address.substr(separator + 1));
            return connection_->Connect(error);
        }

        rdma::RdmaServer& server()
        {
            return *server_;
        }

        rdma::RdmaConnection& connection()
        {
            return *connection_;
        }

    private:
        std::unique_ptr<rdma::RdmaServer> server_;
        std::unique_ptr<rdma::RdmaConnection> connection_;
    };

    bool WaitForPendingRpcs(rdma::RdmaConnection& connection,
                            std::size_t expected,
                            std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (connection.pending_rpc_count() == expected)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return connection.pending_rpc_count() == expected;
    }

    TEST(RdmaTransportContractTest, RdmaRpcControllerContract)
    {
        rdma::RdmaRpcController controller;

        EXPECT_FALSE(controller.Failed());
        EXPECT_FALSE(controller.IsCanceled());
        EXPECT_TRUE(controller.ErrorText().empty());

        controller.SetFailed("controller failure");
        EXPECT_TRUE(controller.Failed());
        EXPECT_EQ(controller.ErrorText(), "controller failure");

        controller.Reset();
        EXPECT_FALSE(controller.Failed());
        EXPECT_FALSE(controller.IsCanceled());
        EXPECT_TRUE(controller.ErrorText().empty());
    }

    TEST(RdmaTransportContractTest, AsyncProbeCallReturnsBeforeServerCompletes)
    {
        if (!HasRdmaDevice())
        {
            GTEST_SKIP() << "no RDMA device is available in this environment";
        }

        DeferredProbeService service;
        LoopbackPair loopback;
        std::string error;
        ASSERT_TRUE(loopback.Start(&service, &error)) << error;

        raft::rdma::RdmaControlService::Stub stub(&loopback.connection());
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(11);
        raft::rdma::ProbeResponse response;
        WaitingClosure done;

        stub.Probe(&controller, &request, &response, &done);

        EXPECT_EQ(done.runs(), 0);
        EXPECT_EQ(loopback.connection().pending_rpc_count(), 1u);
        ASSERT_TRUE(service.WaitForPending(1, kAsyncTimeout));

        ASSERT_TRUE(service.Complete(11, "deferred-ok"));
        ASSERT_TRUE(done.WaitForRuns(1, kAsyncTimeout));

        EXPECT_EQ(done.runs(), 1);
        EXPECT_FALSE(controller.Failed());
        EXPECT_EQ(response.probe_id(), 11u);
        EXPECT_TRUE(response.ok());
        EXPECT_EQ(response.message(), "deferred-ok");
        EXPECT_TRUE(WaitForPendingRpcs(loopback.connection(), 0, kAsyncTimeout));
    }

    TEST(RdmaTransportContractTest, OutOfOrderResponsesMatchTheirRequestIds)
    {
        if (!HasRdmaDevice())
        {
            GTEST_SKIP() << "no RDMA device is available in this environment";
        }

        DeferredProbeService service;
        LoopbackPair loopback;
        std::string error;
        ASSERT_TRUE(loopback.Start(&service, &error)) << error;

        raft::rdma::RdmaControlService::Stub stub(&loopback.connection());
        TestRpcController controllers[3];
        raft::rdma::ProbeRequest requests[3];
        raft::rdma::ProbeResponse responses[3];
        WaitingClosure dones[3];
        const uint64_t ids[3] = {101, 102, 103};

        for (int index = 0; index < 3; ++index)
        {
            requests[index].set_probe_id(ids[index]);
            stub.Probe(&controllers[index], &requests[index], &responses[index], &dones[index]);
        }

        ASSERT_TRUE(service.WaitForPending(3, kAsyncTimeout));
        EXPECT_TRUE(WaitForPendingRpcs(loopback.connection(), 3, kAsyncTimeout));

        ASSERT_TRUE(service.Complete(103, "response-103"));
        ASSERT_TRUE(service.Complete(101, "response-101"));
        ASSERT_TRUE(service.Complete(102, "response-102"));

        for (int index = 0; index < 3; ++index)
        {
            ASSERT_TRUE(dones[index].WaitForRuns(1, kAsyncTimeout));
            EXPECT_EQ(dones[index].runs(), 1);
            EXPECT_EQ(responses[index].probe_id(), ids[index]);
            EXPECT_EQ(responses[index].message(), "response-" + std::to_string(ids[index]));
        }

        EXPECT_TRUE(WaitForPendingRpcs(loopback.connection(), 0, kAsyncTimeout));
    }

    TEST(RdmaTransportContractTest, ManyPendingCallsKeepRequestResourcesAlive)
    {
        if (!HasRdmaDevice())
        {
            GTEST_SKIP() << "no RDMA device is available in this environment";
        }

        constexpr int kCallCount = 32;

        DeferredProbeService service;
        LoopbackPair loopback;
        std::string error;
        ASSERT_TRUE(loopback.Start(&service, &error)) << error;

        raft::rdma::RdmaControlService::Stub stub(&loopback.connection());
        std::vector<std::unique_ptr<TestRpcController>> controllers;
        std::vector<std::unique_ptr<raft::rdma::ProbeRequest>> requests;
        std::vector<std::unique_ptr<raft::rdma::ProbeResponse>> responses;
        std::vector<std::unique_ptr<WaitingClosure>> dones;
        controllers.reserve(kCallCount);
        requests.reserve(kCallCount);
        responses.reserve(kCallCount);
        dones.reserve(kCallCount);

        for (int index = 0; index < kCallCount; ++index)
        {
            controllers.push_back(std::make_unique<TestRpcController>());
            requests.push_back(std::make_unique<raft::rdma::ProbeRequest>());
            responses.push_back(std::make_unique<raft::rdma::ProbeResponse>());
            dones.push_back(std::make_unique<WaitingClosure>());

            const uint64_t probe_id = static_cast<uint64_t>(1000 + index);
            requests.back()->set_probe_id(probe_id);
            stub.Probe(controllers.back().get(),
                       requests.back().get(),
                       responses.back().get(),
                       dones.back().get());
        }

        ASSERT_TRUE(service.WaitForPending(kCallCount, kAsyncTimeout));
        EXPECT_TRUE(WaitForPendingRpcs(loopback.connection(), kCallCount, kAsyncTimeout));

        for (int index = kCallCount - 1; index >= 0; --index)
        {
            const uint64_t probe_id = static_cast<uint64_t>(1000 + index);
            ASSERT_TRUE(service.Complete(probe_id, "response-" + std::to_string(probe_id)));
        }

        for (int index = 0; index < kCallCount; ++index)
        {
            ASSERT_TRUE(dones[index]->WaitForRuns(1, kAsyncTimeout));
            EXPECT_EQ(dones[index]->runs(), 1);
            EXPECT_EQ(responses[index]->probe_id(), static_cast<uint64_t>(1000 + index));
        }

        EXPECT_TRUE(WaitForPendingRpcs(loopback.connection(), 0, kAsyncTimeout));
    }

    TEST(RdmaTransportContractTest, DuplicateResponseForCompletedRequestIsIgnored)
    {
        if (!HasRdmaDevice())
        {
            GTEST_SKIP() << "no RDMA device is available in this environment";
        }

        DeferredProbeService service;
        LoopbackPair loopback;
        std::string error;
        ASSERT_TRUE(loopback.Start(&service, &error)) << error;

        raft::rdma::RdmaControlService::Stub stub(&loopback.connection());
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(55);
        raft::rdma::ProbeResponse response;
        WaitingClosure done;

        stub.Probe(&controller, &request, &response, &done);
        ASSERT_TRUE(service.WaitForPending(1, kAsyncTimeout));
        ASSERT_TRUE(service.Complete(55, "first"));
        ASSERT_TRUE(done.WaitForRuns(1, kAsyncTimeout));

        ASSERT_TRUE(service.ReplayCompletedClosure(55));
        std::this_thread::sleep_for(kSettleDelay);

        EXPECT_EQ(done.runs(), 1);
        EXPECT_EQ(response.message(), "first");
    }

    TEST(RdmaTransportContractTest, ControllerErrorCompletesDoneExactlyOnce)
    {
        if (!HasRdmaDevice())
        {
            GTEST_SKIP() << "no RDMA device is available in this environment";
        }

        DeferredProbeService service;
        LoopbackPair loopback;
        std::string error;
        ASSERT_TRUE(loopback.Start(&service, &error)) << error;

        raft::rdma::RdmaControlService::Stub stub(&loopback.connection());
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(66);
        raft::rdma::ProbeResponse response;
        WaitingClosure done;

        stub.Probe(&controller, &request, &response, &done);
        ASSERT_TRUE(service.WaitForPending(1, kAsyncTimeout));

        ASSERT_TRUE(service.Fail(66, "injected service failure"));
        ASSERT_TRUE(done.WaitForRuns(1, kAsyncTimeout));

        EXPECT_TRUE(controller.Failed());
        EXPECT_EQ(controller.ErrorText(), "injected service failure");
        EXPECT_EQ(done.runs(), 1);
        EXPECT_TRUE(WaitForPendingRpcs(loopback.connection(), 0, kAsyncTimeout));
    }

    TEST(RdmaTransportContractTest, CallOnClosedConnectionFailsWithoutHanging)
    {
        if (!HasRdmaDevice())
        {
            GTEST_SKIP() << "no RDMA device is available in this environment";
        }

        DeferredProbeService service;
        LoopbackPair loopback;
        std::string error;
        ASSERT_TRUE(loopback.Start(&service, &error)) << error;

        loopback.connection().Close();
        EXPECT_FALSE(loopback.connection().connected());

        raft::rdma::RdmaControlService::Stub stub(&loopback.connection());
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(77);
        raft::rdma::ProbeResponse response;
        WaitingClosure done;

        stub.Probe(&controller, &request, &response, &done);

        ASSERT_TRUE(done.WaitForRuns(1, kAsyncTimeout));
        EXPECT_EQ(done.runs(), 1);
        EXPECT_TRUE(controller.Failed());
        EXPECT_EQ(loopback.connection().pending_rpc_count(), 0u);
    }

    TEST(RdmaTransportContractTest, CloseFailsPendingCallsDeterministically)
    {
        if (!HasRdmaDevice())
        {
            GTEST_SKIP() << "no RDMA device is available in this environment";
        }

        DeferredProbeService service;
        LoopbackPair loopback;
        std::string error;
        ASSERT_TRUE(loopback.Start(&service, &error)) << error;

        raft::rdma::RdmaControlService::Stub stub(&loopback.connection());
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(88);
        raft::rdma::ProbeResponse response;
        WaitingClosure done;

        stub.Probe(&controller, &request, &response, &done);
        ASSERT_TRUE(service.WaitForPending(1, kAsyncTimeout));

        loopback.connection().Close();

        ASSERT_TRUE(done.WaitForRuns(1, kAsyncTimeout));
        EXPECT_EQ(done.runs(), 1);
        EXPECT_TRUE(controller.Failed());
        EXPECT_TRUE(WaitForPendingRpcs(loopback.connection(), 0, kAsyncTimeout));

        loopback.connection().Close();
        std::this_thread::sleep_for(kSettleDelay);
        EXPECT_EQ(done.runs(), 1);
    }

    TEST(RdmaTransportContractTest, ConnectionUsesExactlyOneRcQp)
    {
        if (!HasRdmaDevice())
        {
            GTEST_SKIP() << "no RDMA device is available in this environment";
        }

        DeferredProbeService service;
        LoopbackPair loopback;
        std::string error;
        ASSERT_TRUE(loopback.Start(&service, &error)) << error;

        EXPECT_EQ(loopback.connection().qp_count(), 1u);

        raft::rdma::RdmaControlService::Stub stub(&loopback.connection());
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(99);
        raft::rdma::ProbeResponse response;
        WaitingClosure done;

        stub.Probe(&controller, &request, &response, &done);
        ASSERT_TRUE(service.WaitForPending(1, kAsyncTimeout));
        ASSERT_TRUE(service.Complete(99, "qp-check"));
        ASSERT_TRUE(done.WaitForRuns(1, kAsyncTimeout));

        EXPECT_EQ(loopback.connection().qp_count(), 1u);
        EXPECT_EQ(loopback.connection().pending_rpc_count(), 0u);
    }

    TEST(RdmaTransportContractTest, SequentialCallsReuseConnectionReceivePath)
    {
        if (!HasRdmaDevice())
        {
            GTEST_SKIP() << "no RDMA device is available in this environment";
        }

        DeferredProbeService service;
        LoopbackPair loopback;
        std::string error;
        ASSERT_TRUE(loopback.Start(&service, &error)) << error;

        raft::rdma::RdmaControlService::Stub stub(&loopback.connection());

        for (uint64_t probe_id = 201; probe_id <= 203; ++probe_id)
        {
            TestRpcController controller;
            raft::rdma::ProbeRequest request;
            request.set_probe_id(probe_id);
            raft::rdma::ProbeResponse response;
            WaitingClosure done;

            stub.Probe(&controller, &request, &response, &done);
            ASSERT_TRUE(service.WaitForPending(1, kAsyncTimeout));
            ASSERT_TRUE(service.Complete(probe_id, "sequential-" + std::to_string(probe_id)));
            ASSERT_TRUE(done.WaitForRuns(1, kAsyncTimeout));

            EXPECT_EQ(done.runs(), 1);
            EXPECT_FALSE(controller.Failed());
            EXPECT_EQ(response.probe_id(), probe_id);
            EXPECT_EQ(response.message(), "sequential-" + std::to_string(probe_id));
            EXPECT_TRUE(WaitForPendingRpcs(loopback.connection(), 0, kAsyncTimeout));
        }
    }

    TEST(RdmaTransportContractTest, StoppedServerFailsPendingCallsWithoutCrash)
    {
        if (!HasRdmaDevice())
        {
            GTEST_SKIP() << "no RDMA device is available in this environment";
        }

        DeferredProbeService service;
        LoopbackPair loopback;
        std::string error;
        ASSERT_TRUE(loopback.Start(&service, &error)) << error;

        loopback.server().Stop();

        raft::rdma::RdmaControlService::Stub stub(&loopback.connection());
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(300);
        raft::rdma::ProbeResponse response;
        WaitingClosure done;

        stub.Probe(&controller, &request, &response, &done);

        ASSERT_TRUE(done.WaitForRuns(1, kAsyncTimeout));
        EXPECT_EQ(done.runs(), 1);
        EXPECT_TRUE(controller.Failed());
    }

#elif defined(CQUPT_RDMA_ENABLED) && CQUPT_RDMA_ENABLED

    TEST(RdmaTransportContractTest, Stage1AsyncRpcCasesAwaitImplementation)
    {
        GTEST_SKIP() << "rdma/rdma_connection.h and rdma/rdma_server.h are not implemented yet; "
                        "T005-T007 must land the asynchronous RpcChannel chain before the Stage 1 cases can run";
    }

#endif
}
