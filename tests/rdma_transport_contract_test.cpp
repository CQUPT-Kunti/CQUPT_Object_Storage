#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "rdma/rdma_transport.h"

#include "rdma.pb.h"

#if defined(__has_include)
#if __has_include("rdma/rdma_connection.h")
#include "rdma/rdma_connection.h"
#define CQUPT_RDMA_CONTRACT_HAS_CONNECTION 1
#endif
#if __has_include("rdma/rdma_rpc_controller.h")
#include "rdma/rdma_rpc_controller.h"
#define CQUPT_RDMA_CONTRACT_HAS_CONTROLLER 1
#endif
#if __has_include("rdma/rdma_server.h")
#include "rdma/rdma_server.h"
#define CQUPT_RDMA_CONTRACT_HAS_SERVER 1
#endif
#if __has_include("rdma/rdma_memory_region.h")
#include "rdma/rdma_memory_region.h"
#define CQUPT_RDMA_CONTRACT_HAS_MEMORY_REGION 1
#endif
#if __has_include("store/node/storage_rdma_service.h")
#include "store/node/storage_rdma_service.h"
#include "store/runtime/storage_executor.h"
#define CQUPT_RDMA_CONTRACT_HAS_STORAGE_RDMA_SERVICE 1
#endif
#endif

#if defined(CQUPT_RDMA_ENABLED) && CQUPT_RDMA_ENABLED
#if defined(__has_include)
#if __has_include(<infiniband/verbs.h>)
#include <infiniband/verbs.h>
#define CQUPT_RDMA_CONTRACT_HAS_VERBS 1
#endif
#endif
#endif

#if defined(CQUPT_RDMA_CONTRACT_HAS_CONNECTION) && defined(CQUPT_RDMA_CONTRACT_HAS_SERVER)
#define CQUPT_RDMA_CONTRACT_HAS_ASYNC_IMPL 1
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

    TEST(RdmaTransportContractTest, RdmaControlServiceDescriptorExposesProbeAndPullMethods)
    {
        const google::protobuf::ServiceDescriptor* descriptor = raft::rdma::RdmaControlService::descriptor();

        ASSERT_NE(descriptor, nullptr);
        EXPECT_EQ(descriptor->name(), "RdmaControlService");
        EXPECT_EQ(descriptor->method_count(), 2);
        EXPECT_EQ(descriptor->method(0)->name(), "Probe");
        EXPECT_EQ(descriptor->method(0)->input_type()->name(), "ProbeRequest");
        EXPECT_EQ(descriptor->method(0)->output_type()->name(), "ProbeResponse");

        const google::protobuf::MethodDescriptor* pull = descriptor->FindMethodByName("Pull");
        ASSERT_NE(pull, nullptr);
        EXPECT_EQ(pull->input_type()->name(), "PullRequest");
        EXPECT_EQ(pull->output_type()->name(), "PullResponse");
        EXPECT_EQ(descriptor->FindMethodByName("Missing"), nullptr);
    }

    class PullRecordingService final : public raft::rdma::RdmaControlService
    {
    public:
        void Pull(google::protobuf::RpcController*,
                  const raft::rdma::PullRequest* request,
                  raft::rdma::PullResponse* response,
                  google::protobuf::Closure* done) override
        {
            ++calls_;
            response->set_ok(true);
            response->set_durable(true);
            response->set_chunk_id(request->chunk_id());
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

    TEST(RdmaTransportContractTest, GeneratedServiceDispatchInvokesPull)
    {
        PullRecordingService service;
        TestRpcController controller;
        raft::rdma::PullRequest request;
        request.set_chunk_id("chunk-pull");
        raft::rdma::PullResponse response;
        CountingClosure done;

        const google::protobuf::MethodDescriptor* method =
            raft::rdma::RdmaControlService::descriptor()->FindMethodByName("Pull");
        ASSERT_NE(method, nullptr);

        service.CallMethod(method, &controller, &request, &response, &done);

        EXPECT_EQ(service.calls(), 1);
        EXPECT_EQ(done.runs.load(), 1);
        EXPECT_TRUE(response.ok());
        EXPECT_TRUE(response.durable());
        EXPECT_EQ(response.chunk_id(), "chunk-pull");
        EXPECT_FALSE(controller.Failed());
    }

    TEST(RdmaTransportContractTest, GeneratedStubDispatchesPullThroughRpcChannel)
    {
        FakeRpcChannel channel;
        raft::rdma::RdmaControlService::Stub stub(&channel);
        TestRpcController controller;
        raft::rdma::PullRequest request;
        request.set_chunk_id("chunk-dispatch");
        request.set_remote_rkey(7);
        raft::rdma::PullResponse response;
        CountingClosure done;

        stub.Pull(&controller, &request, &response, &done);

        ASSERT_NE(channel.method(), nullptr);
        EXPECT_EQ(channel.method()->full_name(), "raft.rdma.RdmaControlService.Pull");
        EXPECT_EQ(channel.call_count(), 1);
        EXPECT_EQ(done.runs.load(), 0);

        const auto* captured = dynamic_cast<const raft::rdma::PullRequest*>(channel.request());
        ASSERT_NE(captured, nullptr);
        EXPECT_EQ(captured->chunk_id(), "chunk-dispatch");
        EXPECT_EQ(captured->remote_rkey(), 7u);
    }

    TEST(RdmaTransportContractTest, PullResponseExpressesDurableSuccessAndFailure)
    {
        raft::rdma::PullResponse success;
        success.set_ok(true);
        success.set_durable(true);
        success.set_already_exists(false);
        success.set_chunk_id("chunk-a");

        std::string success_bytes;
        ASSERT_TRUE(success.SerializeToString(&success_bytes));

        raft::rdma::PullResponse parsed_success;
        ASSERT_TRUE(parsed_success.ParseFromString(success_bytes));
        EXPECT_TRUE(parsed_success.ok());
        EXPECT_TRUE(parsed_success.durable());
        EXPECT_FALSE(parsed_success.already_exists());
        EXPECT_EQ(parsed_success.chunk_id(), "chunk-a");
        EXPECT_TRUE(parsed_success.error_detail().empty());

        raft::rdma::PullResponse failure;
        failure.set_ok(false);
        failure.set_durable(false);
        failure.set_error_detail("checksum mismatch");

        std::string failure_bytes;
        ASSERT_TRUE(failure.SerializeToString(&failure_bytes));

        raft::rdma::PullResponse parsed_failure;
        ASSERT_TRUE(parsed_failure.ParseFromString(failure_bytes));
        EXPECT_FALSE(parsed_failure.ok());
        EXPECT_FALSE(parsed_failure.durable());
        EXPECT_EQ(parsed_failure.error_detail(), "checksum mismatch");
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

#if defined(CQUPT_RDMA_CONTRACT_HAS_CONNECTION)

    std::uint8_t ReadUint8(const std::string& data, std::size_t offset)
    {
        return static_cast<std::uint8_t>(data[offset]);
    }

    std::uint16_t ReadUint16(const std::string& data, std::size_t offset)
    {
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(ReadUint8(data, offset)) |
            (static_cast<std::uint16_t>(ReadUint8(data, offset + 1)) << 8));
    }

    std::uint32_t ReadUint32(const std::string& data, std::size_t offset)
    {
        return static_cast<std::uint32_t>(
            static_cast<std::uint32_t>(ReadUint8(data, offset)) |
            (static_cast<std::uint32_t>(ReadUint8(data, offset + 1)) << 8) |
            (static_cast<std::uint32_t>(ReadUint8(data, offset + 2)) << 16) |
            (static_cast<std::uint32_t>(ReadUint8(data, offset + 3)) << 24));
    }

    std::uint64_t ReadUint64(const std::string& data, std::size_t offset)
    {
        return static_cast<std::uint64_t>(ReadUint32(data, offset)) |
               (static_cast<std::uint64_t>(ReadUint32(data, offset + 4)) << 32);
    }

    void AppendUint(std::string& out, std::uint64_t value, int byte_count)
    {
        for (int index = 0; index < byte_count; ++index)
        {
            out.push_back(static_cast<char>((value >> (8 * index)) & 0xFFu));
        }
    }

    std::uint64_t RequestIdFromFrame(const std::string& frame)
    {
        return ReadUint64(frame, 2);
    }

    std::string BuildResponseFrame(std::uint64_t request_id,
                                   const std::string& payload,
                                   std::uint32_t status = 0)
    {
        std::string frame;
        AppendUint(frame, 0, 2);
        AppendUint(frame, request_id, 8);
        AppendUint(frame, static_cast<std::uint64_t>(payload.size()), 4);
        AppendUint(frame, status, 4);
        frame.append(payload);
        return frame;
    }

    std::string SerializeProbeResponse(std::uint64_t probe_id, const std::string& message)
    {
        raft::rdma::ProbeResponse payload;
        payload.set_probe_id(probe_id);
        payload.set_ok(true);
        payload.set_message(message);
        std::string bytes;
        payload.SerializeToString(&bytes);
        return bytes;
    }

    class RecordingRdmaConnection final : public rdma::RdmaConnection
    {
    public:
        RecordingRdmaConnection() : rdma::RdmaConnection("127.0.0.1", "0")
        {
        }

        using rdma::RdmaConnection::DrainPendingRpcs;
        using rdma::RdmaConnection::RegisterPendingRpc;
        using rdma::RdmaConnection::TakePendingRpc;

        const std::string& captured_frame() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return frames_.back();
        }

        std::vector<std::string> captured_frames() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return frames_;
        }

        int submissions() const
        {
            return submissions_.load();
        }

    protected:
        bool SubmitFramedRequest(std::string framed_request, std::string* error) override
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                frames_.push_back(std::move(framed_request));
            }
            submissions_.fetch_add(1);
            if (error != nullptr)
            {
                error->clear();
            }
            return true;
        }

    private:
        mutable std::mutex mutex_;
        std::vector<std::string> frames_;
        std::atomic<int> submissions_{0};
    };

    TEST(RdmaTransportContractTest, RdmaConnectionImplementsRpcChannel)
    {
        static_assert(std::is_base_of<google::protobuf::RpcChannel, rdma::RdmaConnection>::value,
                      "RdmaConnection must implement google::protobuf::RpcChannel");
        EXPECT_TRUE((std::is_base_of<google::protobuf::RpcChannel, rdma::RdmaConnection>::value));
    }

    TEST(RdmaTransportContractTest, RdmaConnectionSerializesAndFramesProbeRequest)
    {
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(123);
        raft::rdma::ProbeResponse response;
        CountingClosure done;
        RecordingRdmaConnection connection;
        raft::rdma::RdmaControlService::Stub stub(&connection);

        stub.Probe(&controller, &request, &response, &done);

        EXPECT_EQ(connection.submissions(), 1);
        EXPECT_EQ(done.runs.load(), 0);
        EXPECT_FALSE(controller.Failed());

        const std::string& frame = connection.captured_frame();
        constexpr std::size_t kHeaderSize = 14;
        ASSERT_GE(frame.size(), kHeaderSize);
        EXPECT_EQ(ReadUint16(frame, 0), 0u);

        const std::uint32_t payload_length = ReadUint32(frame, 10);
        ASSERT_EQ(frame.size(), kHeaderSize + payload_length);

        raft::rdma::ProbeRequest decoded;
        ASSERT_TRUE(decoded.ParseFromArray(frame.data() + kHeaderSize, static_cast<int>(payload_length)));
        EXPECT_EQ(decoded.probe_id(), 123u);
        EXPECT_EQ(response.probe_id(), 0u);
    }

    TEST(RdmaTransportContractTest, RdmaConnectionReportsSubmitFailureWithoutHanging)
    {
        rdma::RdmaConnection connection("127.0.0.1", "0");
        raft::rdma::RdmaControlService::Stub stub(&connection);
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(321);
        raft::rdma::ProbeResponse response;
        CountingClosure done;

        stub.Probe(&controller, &request, &response, &done);

        EXPECT_EQ(done.runs.load(), 1);
        EXPECT_TRUE(controller.Failed());
        EXPECT_FALSE(controller.ErrorText().empty());
        EXPECT_EQ(response.probe_id(), 0u);
    }

    TEST(RdmaTransportContractTest, RdmaConnectionRejectsNullCallArguments)
    {
        rdma::RdmaConnection connection("127.0.0.1", "0");
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        raft::rdma::ProbeResponse response;
        CountingClosure done;

        connection.CallMethod(raft::rdma::RdmaControlService::descriptor()->method(0),
                              &controller,
                              nullptr,
                              &response,
                              &done);

        EXPECT_EQ(done.runs.load(), 1);
        EXPECT_TRUE(controller.Failed());
        EXPECT_FALSE(controller.ErrorText().empty());
    }

    TEST(RdmaTransportContractTest, RdmaConnectionAssignsUniqueIncreasingRequestIds)
    {
        TestRpcController controllers[3];
        raft::rdma::ProbeRequest requests[3];
        raft::rdma::ProbeResponse responses[3];
        CountingClosure dones[3];
        RecordingRdmaConnection connection;
        raft::rdma::RdmaControlService::Stub stub(&connection);

        for (int index = 0; index < 3; ++index)
        {
            requests[index].set_probe_id(static_cast<std::uint64_t>(index + 1));
            stub.Probe(&controllers[index], &requests[index], &responses[index], &dones[index]);
        }

        const std::vector<std::string> frames = connection.captured_frames();
        ASSERT_EQ(frames.size(), 3u);

        std::vector<std::uint64_t> ids;
        ids.reserve(frames.size());
        for (const std::string& frame : frames)
        {
            ASSERT_GE(frame.size(), 14u);
            ids.push_back(ReadUint64(frame, 2));
        }

        EXPECT_GE(ids[0], 1u);
        EXPECT_LT(ids[0], ids[1]);
        EXPECT_LT(ids[1], ids[2]);
        EXPECT_EQ(connection.pending_rpc_count(), 3u);
        for (int index = 0; index < 3; ++index)
        {
            EXPECT_EQ(dones[index].runs.load(), 0);
        }
    }

    TEST(RdmaTransportContractTest, RdmaConnectionRegistersPendingRpcBeforeSubmitAndTakesOnce)
    {
        RecordingRdmaConnection connection;
        raft::rdma::RdmaControlService::Stub stub(&connection);
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(55);
        raft::rdma::ProbeResponse response;
        CountingClosure done;

        stub.Probe(&controller, &request, &response, &done);

        ASSERT_EQ(connection.submissions(), 1);
        ASSERT_EQ(connection.pending_rpc_count(), 1u);

        const std::vector<std::string> frames = connection.captured_frames();
        ASSERT_EQ(frames.size(), 1u);
        const std::uint64_t request_id = ReadUint64(frames.front(), 2);

        const std::optional<rdma::PendingRpc> taken = connection.TakePendingRpc(request_id);
        ASSERT_TRUE(taken.has_value());
        EXPECT_EQ(taken->request_id, request_id);
        EXPECT_EQ(taken->controller, &controller);
        EXPECT_EQ(taken->response, &response);
        EXPECT_EQ(taken->done, &done);
        EXPECT_EQ(done.runs.load(), 0);

        EXPECT_FALSE(connection.TakePendingRpc(request_id).has_value());
        EXPECT_EQ(connection.pending_rpc_count(), 0u);
    }

    TEST(RdmaTransportContractTest, RdmaConnectionKeepsPendingRpcAfterSuccessfulSubmit)
    {
        RecordingRdmaConnection connection;
        raft::rdma::RdmaControlService::Stub stub(&connection);
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(66);
        raft::rdma::ProbeResponse response;
        CountingClosure done;

        stub.Probe(&controller, &request, &response, &done);

        EXPECT_EQ(connection.submissions(), 1);
        EXPECT_EQ(connection.pending_rpc_count(), 1u);
        EXPECT_EQ(done.runs.load(), 0);

        const std::vector<rdma::PendingRpc> drained = connection.DrainPendingRpcs();
        EXPECT_EQ(drained.size(), 1u);
        EXPECT_EQ(done.runs.load(), 0);
    }

    TEST(RdmaTransportContractTest, RdmaConnectionRegistersAndTakesPendingRpcExactlyOnce)
    {
        RecordingRdmaConnection connection;
        TestRpcController controller;
        raft::rdma::ProbeResponse response;
        CountingClosure done;

        rdma::PendingRpc pending;
        pending.request_id = 42;
        pending.controller = &controller;
        pending.response = &response;
        pending.done = &done;

        EXPECT_TRUE(connection.RegisterPendingRpc(pending));
        EXPECT_FALSE(connection.RegisterPendingRpc(pending));
        EXPECT_EQ(connection.pending_rpc_count(), 1u);

        const std::optional<rdma::PendingRpc> taken = connection.TakePendingRpc(42);
        ASSERT_TRUE(taken.has_value());
        EXPECT_EQ(taken->request_id, 42u);
        EXPECT_FALSE(connection.TakePendingRpc(42).has_value());
        EXPECT_EQ(connection.pending_rpc_count(), 0u);
    }

    TEST(RdmaTransportContractTest, RdmaConnectionConcurrentCallsProduceDistinctPendingIds)
    {
        constexpr int kThreadCount = 4;
        constexpr int kCallsPerThread = 25;
        constexpr int kTotalCalls = kThreadCount * kCallsPerThread;

        std::vector<std::unique_ptr<TestRpcController>> controllers;
        std::vector<std::unique_ptr<raft::rdma::ProbeRequest>> requests;
        std::vector<std::unique_ptr<raft::rdma::ProbeResponse>> responses;
        std::vector<std::unique_ptr<CountingClosure>> dones;
        for (int index = 0; index < kTotalCalls; ++index)
        {
            controllers.push_back(std::make_unique<TestRpcController>());
            requests.push_back(std::make_unique<raft::rdma::ProbeRequest>());
            responses.push_back(std::make_unique<raft::rdma::ProbeResponse>());
            dones.push_back(std::make_unique<CountingClosure>());
        }

        RecordingRdmaConnection connection;
        std::atomic<bool> start{false};
        std::vector<std::thread> threads;
        threads.reserve(kThreadCount);

        for (int thread_index = 0; thread_index < kThreadCount; ++thread_index)
        {
            threads.emplace_back([&connection, &start, &controllers, &requests, &responses, &dones, thread_index]
                                 {
                                     while (!start.load())
                                     {
                                     }
                                     raft::rdma::RdmaControlService::Stub stub(&connection);
                                     for (int call = 0; call < kCallsPerThread; ++call)
                                     {
                                         const int index = thread_index * kCallsPerThread + call;
                                         requests[index]->set_probe_id(static_cast<std::uint64_t>(index + 1));
                                         stub.Probe(controllers[index].get(),
                                                    requests[index].get(),
                                                    responses[index].get(),
                                                    dones[index].get());
                                     }
                                 });
        }

        start.store(true);
        for (std::thread& thread : threads)
        {
            thread.join();
        }

        EXPECT_EQ(connection.pending_rpc_count(), static_cast<std::size_t>(kTotalCalls));

        const std::vector<std::string> frames = connection.captured_frames();
        ASSERT_EQ(frames.size(), static_cast<std::size_t>(kTotalCalls));

        std::vector<std::uint64_t> ids;
        ids.reserve(frames.size());
        for (const std::string& frame : frames)
        {
            ASSERT_GE(frame.size(), 14u);
            ids.push_back(ReadUint64(frame, 2));
        }
        std::sort(ids.begin(), ids.end());
        EXPECT_GE(ids.front(), 1u);
        EXPECT_EQ(std::adjacent_find(ids.begin(), ids.end()), ids.end());
    }

    TEST(RdmaTransportContractTest, RdmaConnectionRemovesPendingRpcAfterSubmitFailure)
    {
        rdma::RdmaConnection connection("127.0.0.1", "0");
        raft::rdma::RdmaControlService::Stub stub(&connection);
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(77);
        raft::rdma::ProbeResponse response;
        CountingClosure done;

        stub.Probe(&controller, &request, &response, &done);

        EXPECT_EQ(done.runs.load(), 1);
        EXPECT_TRUE(controller.Failed());
        EXPECT_FALSE(controller.ErrorText().empty());
        EXPECT_EQ(connection.pending_rpc_count(), 0u);
    }

    TEST(RdmaTransportContractTest, RdmaConnectionCloseFailsAllPendingRpcsExactlyOnce)
    {
        RecordingRdmaConnection connection;
        raft::rdma::RdmaControlService::Stub stub(&connection);
        TestRpcController controllers[3];
        raft::rdma::ProbeRequest requests[3];
        raft::rdma::ProbeResponse responses[3];
        CountingClosure dones[3];

        for (int index = 0; index < 3; ++index)
        {
            requests[index].set_probe_id(static_cast<std::uint64_t>(100 + index));
            stub.Probe(&controllers[index], &requests[index], &responses[index], &dones[index]);
        }
        ASSERT_EQ(connection.pending_rpc_count(), 3u);

        connection.Close();

        EXPECT_EQ(connection.pending_rpc_count(), 0u);
        for (int index = 0; index < 3; ++index)
        {
            EXPECT_EQ(dones[index].runs.load(), 1);
            EXPECT_TRUE(controllers[index].Failed());
            EXPECT_FALSE(controllers[index].ErrorText().empty());
        }

        connection.Close();
        for (int index = 0; index < 3; ++index)
        {
            EXPECT_EQ(dones[index].runs.load(), 1);
        }
    }

    TEST(RdmaTransportContractTest, RdmaConnectionRejectsNewRpcAfterClose)
    {
        RecordingRdmaConnection connection;
        connection.Close();

        raft::rdma::RdmaControlService::Stub stub(&connection);
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(88);
        raft::rdma::ProbeResponse response;
        CountingClosure done;

        stub.Probe(&controller, &request, &response, &done);

        EXPECT_EQ(done.runs.load(), 1);
        EXPECT_TRUE(controller.Failed());
        EXPECT_FALSE(controller.ErrorText().empty());
        EXPECT_EQ(connection.pending_rpc_count(), 0u);
        EXPECT_EQ(connection.submissions(), 0);
    }

    TEST(RdmaTransportContractTest, RdmaConnectionDispatchesResponseAndRunsDoneOnce)
    {
        TestRpcController controller;
        raft::rdma::ProbeResponse response;
        CountingClosure done;
        RecordingRdmaConnection connection;
        raft::rdma::RdmaControlService::Stub stub(&connection);
        raft::rdma::ProbeRequest request;
        request.set_probe_id(1001);

        stub.Probe(&controller, &request, &response, &done);

        EXPECT_EQ(done.runs.load(), 0);
        ASSERT_EQ(connection.pending_rpc_count(), 1u);

        const std::uint64_t request_id = RequestIdFromFrame(connection.captured_frame());
        const std::string frame = BuildResponseFrame(request_id, SerializeProbeResponse(1001, "matched"));

        connection.OnRpcResponse(frame.data(), frame.size());

        EXPECT_EQ(done.runs.load(), 1);
        EXPECT_FALSE(controller.Failed());
        EXPECT_EQ(response.probe_id(), 1001u);
        EXPECT_TRUE(response.ok());
        EXPECT_EQ(response.message(), "matched");
        EXPECT_EQ(connection.pending_rpc_count(), 0u);

        connection.OnRpcResponse(frame.data(), frame.size());
        EXPECT_EQ(done.runs.load(), 1);
        EXPECT_EQ(response.message(), "matched");
    }

    TEST(RdmaTransportContractTest, RdmaConnectionMatchesOutOfOrderResponsesByRequestId)
    {
        TestRpcController controllers[3];
        raft::rdma::ProbeResponse responses[3];
        CountingClosure dones[3];
        RecordingRdmaConnection connection;
        raft::rdma::RdmaControlService::Stub stub(&connection);
        raft::rdma::ProbeRequest requests[3];

        for (int index = 0; index < 3; ++index)
        {
            requests[index].set_probe_id(static_cast<std::uint64_t>(index + 1));
            stub.Probe(&controllers[index], &requests[index], &responses[index], &dones[index]);
        }

        const std::vector<std::string> frames = connection.captured_frames();
        ASSERT_EQ(frames.size(), 3u);

        std::uint64_t ids[3];
        for (int index = 0; index < 3; ++index)
        {
            ids[index] = RequestIdFromFrame(frames[index]);
        }

        const int response_order[3] = {2, 0, 1};
        for (int step = 0; step < 3; ++step)
        {
            const int index = response_order[step];
            const std::string frame = BuildResponseFrame(
                ids[index],
                SerializeProbeResponse(static_cast<std::uint64_t>(index + 1), "response-" + std::to_string(index + 1)));
            connection.OnRpcResponse(frame.data(), frame.size());
        }

        for (int index = 0; index < 3; ++index)
        {
            EXPECT_EQ(dones[index].runs.load(), 1);
            EXPECT_FALSE(controllers[index].Failed());
            EXPECT_EQ(responses[index].probe_id(), static_cast<std::uint64_t>(index + 1));
            EXPECT_EQ(responses[index].message(), "response-" + std::to_string(index + 1));
        }
        EXPECT_EQ(connection.pending_rpc_count(), 0u);
    }

    TEST(RdmaTransportContractTest, RdmaConnectionDropsUnknownRequestIdResponse)
    {
        TestRpcController controller;
        raft::rdma::ProbeResponse response;
        CountingClosure done;
        RecordingRdmaConnection connection;
        raft::rdma::RdmaControlService::Stub stub(&connection);
        raft::rdma::ProbeRequest request;
        request.set_probe_id(5);

        stub.Probe(&controller, &request, &response, &done);

        const std::uint64_t request_id = RequestIdFromFrame(connection.captured_frame());

        const std::string unknown = BuildResponseFrame(request_id + 1000, SerializeProbeResponse(5, "unknown"));
        connection.OnRpcResponse(unknown.data(), unknown.size());

        EXPECT_EQ(done.runs.load(), 0);
        EXPECT_FALSE(controller.Failed());
        EXPECT_EQ(response.probe_id(), 0u);
        EXPECT_EQ(connection.pending_rpc_count(), 1u);

        const std::string matching = BuildResponseFrame(request_id, SerializeProbeResponse(5, "known"));
        connection.OnRpcResponse(matching.data(), matching.size());

        EXPECT_EQ(done.runs.load(), 1);
        EXPECT_FALSE(controller.Failed());
        EXPECT_EQ(response.message(), "known");
        EXPECT_EQ(connection.pending_rpc_count(), 0u);
    }

    TEST(RdmaTransportContractTest, RdmaConnectionFailsMalformedPayloadWithoutAffectingOtherRpc)
    {
        TestRpcController first_controller;
        raft::rdma::ProbeResponse first_response;
        CountingClosure first_done;
        TestRpcController second_controller;
        raft::rdma::ProbeResponse second_response;
        CountingClosure second_done;
        RecordingRdmaConnection connection;
        raft::rdma::RdmaControlService::Stub stub(&connection);
        raft::rdma::ProbeRequest first_request;
        first_request.set_probe_id(1);
        raft::rdma::ProbeRequest second_request;
        second_request.set_probe_id(2);

        stub.Probe(&first_controller, &first_request, &first_response, &first_done);
        stub.Probe(&second_controller, &second_request, &second_response, &second_done);

        const std::vector<std::string> frames = connection.captured_frames();
        ASSERT_EQ(frames.size(), 2u);
        const std::uint64_t first_id = RequestIdFromFrame(frames[0]);
        const std::uint64_t second_id = RequestIdFromFrame(frames[1]);

        const std::string malformed_payload(1, static_cast<char>(0x08));
        const std::string malformed = BuildResponseFrame(first_id, malformed_payload);
        connection.OnRpcResponse(malformed.data(), malformed.size());

        EXPECT_EQ(first_done.runs.load(), 1);
        EXPECT_TRUE(first_controller.Failed());
        EXPECT_FALSE(first_controller.ErrorText().empty());

        const std::string valid = BuildResponseFrame(second_id, SerializeProbeResponse(2, "second-ok"));
        connection.OnRpcResponse(valid.data(), valid.size());

        EXPECT_EQ(second_done.runs.load(), 1);
        EXPECT_FALSE(second_controller.Failed());
        EXPECT_EQ(second_response.probe_id(), 2u);
        EXPECT_EQ(second_response.message(), "second-ok");
        EXPECT_EQ(connection.pending_rpc_count(), 0u);
    }

    TEST(RdmaTransportContractTest, RdmaConnectionDropsIllegalResponseHeaderSafely)
    {
        TestRpcController controller;
        raft::rdma::ProbeResponse response;
        CountingClosure done;
        RecordingRdmaConnection connection;
        raft::rdma::RdmaControlService::Stub stub(&connection);
        raft::rdma::ProbeRequest request;
        request.set_probe_id(3);

        stub.Probe(&controller, &request, &response, &done);

        const std::uint64_t request_id = RequestIdFromFrame(connection.captured_frame());

        const char short_header[] = {0x00, 0x01, 0x02};
        connection.OnRpcResponse(short_header, sizeof(short_header));
        connection.OnRpcResponse(nullptr, 0);

        EXPECT_EQ(done.runs.load(), 0);
        EXPECT_FALSE(controller.Failed());
        EXPECT_EQ(connection.pending_rpc_count(), 1u);

        std::string mismatch;
        AppendUint(mismatch, 0, 2);
        AppendUint(mismatch, request_id, 8);
        AppendUint(mismatch, 5, 4);
        AppendUint(mismatch, 0, 4);
        mismatch.append("ab");
        connection.OnRpcResponse(mismatch.data(), mismatch.size());

        EXPECT_EQ(done.runs.load(), 1);
        EXPECT_TRUE(controller.Failed());
        EXPECT_FALSE(controller.ErrorText().empty());
        EXPECT_EQ(connection.pending_rpc_count(), 0u);
    }

    TEST(RdmaTransportContractTest, RdmaConnectionCompletesRequestExactlyOnceUnderResponseAndCloseRace)
    {
        for (int iteration = 0; iteration < 20; ++iteration)
        {
            TestRpcController controller;
            raft::rdma::ProbeResponse response;
            CountingClosure done;
            RecordingRdmaConnection connection;
            raft::rdma::RdmaControlService::Stub stub(&connection);
            raft::rdma::ProbeRequest request;
            request.set_probe_id(static_cast<std::uint64_t>(iteration + 1));

            stub.Probe(&controller, &request, &response, &done);

            const std::uint64_t request_id = RequestIdFromFrame(connection.captured_frame());
            const std::string frame = BuildResponseFrame(
                request_id,
                SerializeProbeResponse(static_cast<std::uint64_t>(iteration + 1), "race"));

            std::thread responder([&connection, &frame]
                                  { connection.OnRpcResponse(frame.data(), frame.size()); });
            connection.Close();
            responder.join();

            EXPECT_EQ(done.runs.load(), 1);
            EXPECT_EQ(connection.pending_rpc_count(), 0u);
        }
    }

    TEST(RdmaTransportContractTest, RdmaConnectionTransportFailureFailsAllPendingRpcs)
    {
        TestRpcController controllers[2];
        raft::rdma::ProbeResponse responses[2];
        CountingClosure dones[2];
        RecordingRdmaConnection connection;
        raft::rdma::RdmaControlService::Stub stub(&connection);
        raft::rdma::ProbeRequest requests[2];

        for (int index = 0; index < 2; ++index)
        {
            requests[index].set_probe_id(static_cast<std::uint64_t>(index + 1));
            stub.Probe(&controllers[index], &requests[index], &responses[index], &dones[index]);
        }
        ASSERT_EQ(connection.pending_rpc_count(), 2u);

        connection.OnTransportFailure("asynchronous transport failure");

        EXPECT_EQ(connection.pending_rpc_count(), 0u);
        for (int index = 0; index < 2; ++index)
        {
            EXPECT_EQ(dones[index].runs.load(), 1);
            EXPECT_TRUE(controllers[index].Failed());
            EXPECT_EQ(controllers[index].ErrorText(), "asynchronous transport failure");
        }

        TestRpcController late_controller;
        raft::rdma::ProbeResponse late_response;
        CountingClosure late_done;
        raft::rdma::ProbeRequest late_request;
        late_request.set_probe_id(99);
        stub.Probe(&late_controller, &late_request, &late_response, &late_done);

        EXPECT_EQ(late_done.runs.load(), 1);
        EXPECT_TRUE(late_controller.Failed());
        EXPECT_EQ(connection.submissions(), 2);
    }

#endif

#if defined(CQUPT_RDMA_CONTRACT_HAS_CONTROLLER)

    class ReentrantCancelClosure final : public google::protobuf::Closure
    {
    public:
        explicit ReentrantCancelClosure(rdma::RdmaRpcController& controller)
            : controller_(controller)
        {
        }

        void Run() override
        {
            observed_canceled = controller_.IsCanceled();
            observed_failed = controller_.Failed();
            observed_error = controller_.ErrorText();
            runs.fetch_add(1, std::memory_order_acq_rel);
        }

        std::atomic<int> runs{0};
        std::atomic<bool> observed_canceled{false};
        std::atomic<bool> observed_failed{false};
        std::string observed_error;

    private:
        rdma::RdmaRpcController& controller_;
    };

    TEST(RdmaTransportContractTest, RdmaRpcControllerStartsInCleanState)
    {
        rdma::RdmaRpcController controller;

        EXPECT_FALSE(controller.Failed());
        EXPECT_TRUE(controller.ErrorText().empty());
        EXPECT_FALSE(controller.IsCanceled());
    }

    TEST(RdmaTransportContractTest, RdmaRpcControllerSetFailedStoresReason)
    {
        rdma::RdmaRpcController controller;

        controller.SetFailed("RDMA send failed");

        EXPECT_TRUE(controller.Failed());
        EXPECT_EQ(controller.ErrorText(), "RDMA send failed");
        EXPECT_FALSE(controller.IsCanceled());
    }

    TEST(RdmaTransportContractTest, RdmaRpcControllerResetRestoresInitialState)
    {
        rdma::RdmaRpcController controller;
        controller.SetFailed("RDMA send failed");
        controller.StartCancel();

        CountingClosure callback;
        controller.NotifyOnCancel(&callback);
        EXPECT_EQ(callback.runs.load(), 1);

        controller.Reset();

        EXPECT_FALSE(controller.Failed());
        EXPECT_TRUE(controller.ErrorText().empty());
        EXPECT_FALSE(controller.IsCanceled());

        controller.StartCancel();
        EXPECT_EQ(callback.runs.load(), 1);
    }

    TEST(RdmaTransportContractTest, RdmaRpcControllerStartCancelSetsCanceled)
    {
        rdma::RdmaRpcController controller;

        controller.StartCancel();

        EXPECT_TRUE(controller.IsCanceled());
        EXPECT_FALSE(controller.Failed());
    }

    TEST(RdmaTransportContractTest, RdmaRpcControllerNotifiesCallbacksRegisteredBeforeCancel)
    {
        rdma::RdmaRpcController controller;
        CountingClosure first;
        CountingClosure second;
        controller.NotifyOnCancel(&first);
        controller.NotifyOnCancel(&second);

        controller.StartCancel();

        EXPECT_EQ(first.runs.load(), 1);
        EXPECT_EQ(second.runs.load(), 1);

        controller.StartCancel();
        EXPECT_EQ(first.runs.load(), 1);
        EXPECT_EQ(second.runs.load(), 1);
    }

    TEST(RdmaTransportContractTest, RdmaRpcControllerNotifiesCallbackRegisteredAfterCancel)
    {
        rdma::RdmaRpcController controller;
        controller.StartCancel();

        CountingClosure callback;
        controller.NotifyOnCancel(&callback);

        EXPECT_EQ(callback.runs.load(), 1);

        controller.StartCancel();
        EXPECT_EQ(callback.runs.load(), 1);
    }

    TEST(RdmaTransportContractTest, RdmaRpcControllerRunsCancelCallbackWithoutHoldingLock)
    {
        rdma::RdmaRpcController controller;
        ReentrantCancelClosure callback(controller);
        controller.NotifyOnCancel(&callback);

        controller.StartCancel();

        EXPECT_EQ(callback.runs.load(), 1);
        EXPECT_TRUE(callback.observed_canceled.load());
        EXPECT_FALSE(callback.observed_failed.load());
        EXPECT_TRUE(callback.observed_error.empty());
    }

    TEST(RdmaTransportContractTest, RdmaRpcControllerConcurrentAccessDoesNotRace)
    {
        rdma::RdmaRpcController controller;
        constexpr int kThreadCount = 4;
        constexpr int kIterations = 200;
        std::atomic<bool> start{false};
        std::vector<std::thread> threads;
        threads.reserve(kThreadCount);

        for (int index = 0; index < kThreadCount; ++index)
        {
            threads.emplace_back([&controller, &start, index]
                                 {
                                     while (!start.load())
                                     {
                                     }
                                     for (int iteration = 0; iteration < kIterations; ++iteration)
                                     {
                                         if (index % 2 == 0)
                                         {
                                             controller.SetFailed("failure-" + std::to_string(iteration));
                                         }
                                         else
                                         {
                                             (void)controller.Failed();
                                             (void)controller.ErrorText();
                                         }
                                         (void)controller.IsCanceled();
                                     }
                                 });
        }

        start.store(true);
        for (std::thread& thread : threads)
        {
            thread.join();
        }

        EXPECT_TRUE(controller.Failed());
        EXPECT_FALSE(controller.ErrorText().empty());
        EXPECT_FALSE(controller.IsCanceled());
    }

#if defined(CQUPT_RDMA_CONTRACT_HAS_CONNECTION)

    TEST(RdmaTransportContractTest, RdmaConnectionReportsImmediateFailureThroughRdmaRpcController)
    {
        rdma::RdmaConnection connection("127.0.0.1", "0");
        raft::rdma::RdmaControlService::Stub stub(&connection);
        rdma::RdmaRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(11);
        raft::rdma::ProbeResponse response;
        CountingClosure done;

        stub.Probe(&controller, &request, &response, &done);

        EXPECT_EQ(done.runs.load(), 1);
        EXPECT_TRUE(controller.Failed());
        EXPECT_FALSE(controller.ErrorText().empty());
        EXPECT_EQ(response.probe_id(), 0u);
    }

#endif

#endif

#if defined(CQUPT_RDMA_CONTRACT_HAS_MEMORY_REGION)

    TEST(RdmaTransportContractTest, RdmaMemoryRegionAllocateRejectsZeroSize)
    {
        std::string error;
        const std::unique_ptr<rdma::RdmaMemoryRegion> region = rdma::RdmaMemoryRegion::Allocate(0, &error);

        EXPECT_EQ(region, nullptr);
        EXPECT_FALSE(error.empty());
    }

    TEST(RdmaTransportContractTest, RdmaMemoryRegionOwnsBufferUntilRelease)
    {
        std::string error;
        std::unique_ptr<rdma::RdmaMemoryRegion> region = rdma::RdmaMemoryRegion::Allocate(4096, &error);
        ASSERT_NE(region, nullptr) << error;

        EXPECT_NE(region->data(), nullptr);
        EXPECT_EQ(region->size(), 4096u);
        EXPECT_EQ(region->lkey(), 0u);
        EXPECT_EQ(region->rkey(), 0u);
        EXPECT_EQ(region->native_handle(), nullptr);
        EXPECT_EQ(region->state(), rdma::RdmaMemoryRegionState::kAllocated);

        ASSERT_TRUE(region->Release(&error)) << error;
        EXPECT_EQ(region->state(), rdma::RdmaMemoryRegionState::kReleased);
        EXPECT_EQ(region->data(), nullptr);
        EXPECT_EQ(region->size(), 0u);

        EXPECT_TRUE(region->Release(&error));
        EXPECT_EQ(region->state(), rdma::RdmaMemoryRegionState::kReleased);
    }

    TEST(RdmaTransportContractTest, RdmaMemoryRegionRegisterWithoutProtectionDomainFailsWithoutLeak)
    {
        std::string error;
        std::unique_ptr<rdma::RdmaMemoryRegion> region = rdma::RdmaMemoryRegion::Allocate(1024, &error);
        ASSERT_NE(region, nullptr) << error;

        EXPECT_FALSE(region->Register(nullptr, 0x1, &error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(region->state(), rdma::RdmaMemoryRegionState::kAllocated);
        EXPECT_NE(region->data(), nullptr);
        EXPECT_EQ(region->size(), 1024u);

        error.clear();
        ASSERT_TRUE(region->Release(&error)) << error;
        EXPECT_EQ(region->state(), rdma::RdmaMemoryRegionState::kReleased);
    }

    TEST(RdmaTransportContractTest, RdmaMemoryRegionDeregisterAndReleaseAreIdempotent)
    {
        std::string error;
        std::unique_ptr<rdma::RdmaMemoryRegion> region = rdma::RdmaMemoryRegion::Allocate(256, &error);
        ASSERT_NE(region, nullptr) << error;

        EXPECT_TRUE(region->Deregister(&error));
        EXPECT_TRUE(region->Deregister(&error));
        EXPECT_EQ(region->state(), rdma::RdmaMemoryRegionState::kDeregistered);

        EXPECT_TRUE(region->Release(&error));
        EXPECT_TRUE(region->Release(&error));
        EXPECT_EQ(region->state(), rdma::RdmaMemoryRegionState::kReleased);
    }

#if defined(CQUPT_RDMA_CONTRACT_HAS_VERBS)

    class IbvProtectionDomainScope
    {
    public:
        bool Open(std::string* error)
        {
            int device_count = 0;
            devices_ = ibv_get_device_list(&device_count);
            if (devices_ == nullptr || device_count <= 0)
            {
                if (error != nullptr)
                {
                    *error = "no RDMA device is available in this environment";
                }
                return false;
            }

            context_ = ibv_open_device(devices_[0]);
            if (context_ == nullptr)
            {
                if (error != nullptr)
                {
                    *error = "ibv_open_device failed for the first RDMA device";
                }
                return false;
            }

            pd_ = ibv_alloc_pd(context_);
            if (pd_ == nullptr)
            {
                if (error != nullptr)
                {
                    *error = "ibv_alloc_pd failed";
                }
                return false;
            }
            return true;
        }

        ~IbvProtectionDomainScope()
        {
            if (pd_ != nullptr)
            {
                ibv_dealloc_pd(pd_);
            }
            if (context_ != nullptr)
            {
                ibv_close_device(context_);
            }
            if (devices_ != nullptr)
            {
                ibv_free_device_list(devices_);
            }
        }

        ibv_pd* pd() const
        {
            return pd_;
        }

    private:
        ibv_device** devices_ = nullptr;
        ibv_context* context_ = nullptr;
        ibv_pd* pd_ = nullptr;
    };

    TEST(RdmaTransportContractTest, RdmaMemoryRegionRegistersWithLocalWriteAccess)
    {
        IbvProtectionDomainScope scope;
        std::string error;
        if (!scope.Open(&error))
        {
            GTEST_SKIP() << error;
        }

        std::unique_ptr<rdma::RdmaMemoryRegion> region = rdma::RdmaMemoryRegion::Allocate(4096, &error);
        ASSERT_NE(region, nullptr) << error;

        ASSERT_TRUE(region->Register(scope.pd(), IBV_ACCESS_LOCAL_WRITE, &error)) << error;
        EXPECT_EQ(region->state(), rdma::RdmaMemoryRegionState::kRegistered);
        EXPECT_NE(region->native_handle(), nullptr);
        EXPECT_NE(region->lkey(), 0u);
        EXPECT_NE(region->rkey(), 0u);
        EXPECT_NE(region->data(), nullptr);
        EXPECT_EQ(region->size(), 4096u);

        ASSERT_TRUE(region->Deregister(&error)) << error;
        EXPECT_EQ(region->state(), rdma::RdmaMemoryRegionState::kDeregistered);
        EXPECT_EQ(region->lkey(), 0u);
        EXPECT_EQ(region->rkey(), 0u);
        EXPECT_EQ(region->native_handle(), nullptr);

        ASSERT_TRUE(region->Release(&error)) << error;
        EXPECT_EQ(region->state(), rdma::RdmaMemoryRegionState::kReleased);
    }

    TEST(RdmaTransportContractTest, RdmaMemoryRegionRegistersWithRemoteReadAccessForPullSource)
    {
        IbvProtectionDomainScope scope;
        std::string error;
        if (!scope.Open(&error))
        {
            GTEST_SKIP() << error;
        }

        std::unique_ptr<rdma::RdmaMemoryRegion> region = rdma::RdmaMemoryRegion::Allocate(8192, &error);
        ASSERT_NE(region, nullptr) << error;

        ASSERT_TRUE(region->Register(scope.pd(), IBV_ACCESS_REMOTE_READ, &error)) << error;
        EXPECT_EQ(region->state(), rdma::RdmaMemoryRegionState::kRegistered);
        EXPECT_NE(region->rkey(), 0u);

        ASSERT_TRUE(region->Release(&error)) << error;
        EXPECT_EQ(region->state(), rdma::RdmaMemoryRegionState::kReleased);
        EXPECT_TRUE(region->Release(&error));
    }

    TEST(RdmaTransportContractTest, RdmaMemoryRegionRegistersWithRemoteWriteAccessForPushTarget)
    {
        IbvProtectionDomainScope scope;
        std::string error;
        if (!scope.Open(&error))
        {
            GTEST_SKIP() << error;
        }

        std::unique_ptr<rdma::RdmaMemoryRegion> region = rdma::RdmaMemoryRegion::Allocate(4096, &error);
        ASSERT_NE(region, nullptr) << error;

        ASSERT_TRUE(region->Register(scope.pd(), IBV_ACCESS_REMOTE_WRITE, &error)) << error;
        EXPECT_EQ(region->state(), rdma::RdmaMemoryRegionState::kRegistered);
        EXPECT_NE(region->rkey(), 0u);

        ASSERT_TRUE(region->Release(&error)) << error;
        EXPECT_EQ(region->state(), rdma::RdmaMemoryRegionState::kReleased);
    }

#endif

#endif

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

    class InProcessConnection final : public rdma::RdmaConnection
    {
    public:
        InProcessConnection() : rdma::RdmaConnection("in-process", "0")
        {
        }

        void set_server(rdma::RdmaServer* server)
        {
            server_ = server;
        }

        void DeliverResponse(const std::string& frame)
        {
            OnRpcResponse(frame.data(), frame.size());
        }

        int submissions() const
        {
            return submissions_;
        }

        const std::string& last_request_frame() const
        {
            return last_request_frame_;
        }

    protected:
        bool SubmitFramedRequest(std::string framed_request, std::string* error) override
        {
            last_request_frame_ = framed_request;
            ++submissions_;
            if (server_ == nullptr)
            {
                if (error != nullptr)
                {
                    *error = "no server bound";
                }
                return false;
            }

            server_->OnRpcRequest(framed_request.data(), framed_request.size());
            if (error != nullptr)
            {
                error->clear();
            }
            return true;
        }

    private:
        rdma::RdmaServer* server_ = nullptr;
        std::string last_request_frame_;
        int submissions_ = 0;
    };

    class InProcessServer final : public rdma::RdmaServer
    {
    public:
        explicit InProcessServer(google::protobuf::Service* service)
            : rdma::RdmaServer(service)
        {
        }

        void set_client(InProcessConnection* client)
        {
            client_ = client;
        }

        int responses() const
        {
            return responses_;
        }

        const std::string& last_response_frame() const
        {
            return last_response_frame_;
        }

    protected:
        bool SubmitFramedResponse(std::string framed_response, std::string* error) override
        {
            last_response_frame_ = framed_response;
            ++responses_;
            if (client_ == nullptr)
            {
                if (error != nullptr)
                {
                    *error = "no client bound";
                }
                return false;
            }

            client_->DeliverResponse(framed_response);
            if (error != nullptr)
            {
                error->clear();
            }
            return true;
        }

    private:
        InProcessConnection* client_ = nullptr;
        std::string last_response_frame_;
        int responses_ = 0;
    };

    class FailingProbeService final : public raft::rdma::RdmaControlService
    {
    public:
        void Probe(google::protobuf::RpcController* controller,
                   const raft::rdma::ProbeRequest*,
                   raft::rdma::ProbeResponse*,
                   google::protobuf::Closure* done) override
        {
            controller->SetFailed("service failure");
            if (done != nullptr)
            {
                done->Run();
            }
        }
    };

    std::uint32_t ResponseStatusFromFrame(const std::string& frame)
    {
        return ReadUint32(frame, 14);
    }

    TEST(RdmaTransportContractTest, InProcessSyncServiceRpcCompletesFullChain)
    {
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(42);
        raft::rdma::ProbeResponse response;
        CountingClosure done;
        RecordingProbeService service;
        InProcessServer server(&service);
        InProcessConnection connection;
        connection.set_server(&server);
        server.set_client(&connection);
        raft::rdma::RdmaControlService::Stub stub(&connection);

        stub.Probe(&controller, &request, &response, &done);

        EXPECT_EQ(service.calls(), 1);
        EXPECT_EQ(server.responses(), 1);
        EXPECT_EQ(done.runs.load(), 1);
        EXPECT_FALSE(controller.Failed());
        EXPECT_EQ(response.probe_id(), 42u);
        EXPECT_EQ(connection.pending_rpc_count(), 0u);
        EXPECT_EQ(server.pending_request_count(), 0u);
    }

    TEST(RdmaTransportContractTest, InProcessAsyncServiceSendsResponseOnlyAfterCompletion)
    {
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(77);
        raft::rdma::ProbeResponse response;
        CountingClosure done;
        DeferredProbeService service;
        InProcessServer server(&service);
        InProcessConnection connection;
        connection.set_server(&server);
        server.set_client(&connection);
        raft::rdma::RdmaControlService::Stub stub(&connection);

        stub.Probe(&controller, &request, &response, &done);

        EXPECT_EQ(done.runs.load(), 0);
        EXPECT_EQ(server.responses(), 0);
        EXPECT_EQ(connection.pending_rpc_count(), 1u);
        EXPECT_EQ(server.pending_request_count(), 1u);
        ASSERT_TRUE(service.WaitForPending(1, kAsyncTimeout));

        ASSERT_TRUE(service.Complete(77, "async-ok"));

        EXPECT_EQ(server.responses(), 1);
        EXPECT_EQ(done.runs.load(), 1);
        EXPECT_FALSE(controller.Failed());
        EXPECT_EQ(response.probe_id(), 77u);
        EXPECT_EQ(response.message(), "async-ok");
        EXPECT_EQ(connection.pending_rpc_count(), 0u);
        EXPECT_EQ(server.pending_request_count(), 0u);
    }

    TEST(RdmaTransportContractTest, InProcessConcurrentRequestsMatchResponsesByRequestId)
    {
        constexpr int kCallCount = 8;

        TestRpcController controllers[kCallCount];
        raft::rdma::ProbeRequest requests[kCallCount];
        raft::rdma::ProbeResponse responses[kCallCount];
        CountingClosure dones[kCallCount];
        DeferredProbeService service;
        InProcessServer server(&service);
        InProcessConnection connection;
        connection.set_server(&server);
        server.set_client(&connection);
        raft::rdma::RdmaControlService::Stub stub(&connection);

        for (int index = 0; index < kCallCount; ++index)
        {
            requests[index].set_probe_id(static_cast<std::uint64_t>(index + 1));
            stub.Probe(&controllers[index], &requests[index], &responses[index], &dones[index]);
        }

        ASSERT_TRUE(service.WaitForPending(kCallCount, kAsyncTimeout));
        EXPECT_EQ(server.pending_request_count(), static_cast<std::size_t>(kCallCount));
        EXPECT_EQ(connection.pending_rpc_count(), static_cast<std::size_t>(kCallCount));

        for (int index = kCallCount - 1; index >= 0; --index)
        {
            ASSERT_TRUE(service.Complete(static_cast<std::uint64_t>(index + 1),
                                         "resp-" + std::to_string(index + 1)));
        }

        for (int index = 0; index < kCallCount; ++index)
        {
            EXPECT_EQ(dones[index].runs.load(), 1);
            EXPECT_FALSE(controllers[index].Failed());
            EXPECT_EQ(responses[index].probe_id(), static_cast<std::uint64_t>(index + 1));
            EXPECT_EQ(responses[index].message(), "resp-" + std::to_string(index + 1));
        }
        EXPECT_EQ(server.pending_request_count(), 0u);
        EXPECT_EQ(connection.pending_rpc_count(), 0u);
    }

    TEST(RdmaTransportContractTest, InProcessUnknownMethodReturnsFailureResponse)
    {
        RecordingProbeService service;
        InProcessServer server(&service);

        std::string request_frame;
        AppendUint(request_frame, 7, 2);
        AppendUint(request_frame, 4242, 8);
        AppendUint(request_frame, 0, 4);

        server.OnRpcRequest(request_frame.data(), request_frame.size());

        EXPECT_EQ(service.calls(), 0);
        EXPECT_EQ(server.responses(), 1);
        const std::string& response = server.last_response_frame();
        ASSERT_GE(response.size(), 18u);
        EXPECT_EQ(ReadUint64(response, 2), 4242u);
        EXPECT_NE(ResponseStatusFromFrame(response), 0u);
        EXPECT_EQ(server.pending_request_count(), 0u);
    }

    TEST(RdmaTransportContractTest, InProcessMalformedRequestIsRejectedWithoutCallingService)
    {
        RecordingProbeService service;
        InProcessServer server(&service);

        std::string mismatch;
        AppendUint(mismatch, 0, 2);
        AppendUint(mismatch, 99, 8);
        AppendUint(mismatch, 5, 4);
        mismatch.append("ab");

        server.OnRpcRequest(mismatch.data(), mismatch.size());

        EXPECT_EQ(service.calls(), 0);
        EXPECT_EQ(server.responses(), 1);
        EXPECT_EQ(ReadUint64(server.last_response_frame(), 2), 99u);
        EXPECT_NE(ResponseStatusFromFrame(server.last_response_frame()), 0u);

        server.OnRpcRequest(nullptr, 0);
        const char short_frame[] = {0x01, 0x02, 0x03};
        server.OnRpcRequest(short_frame, sizeof(short_frame));

        EXPECT_EQ(service.calls(), 0);
        EXPECT_EQ(server.responses(), 1);
    }

    TEST(RdmaTransportContractTest, InProcessServiceFailurePropagatesToClientController)
    {
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(5);
        raft::rdma::ProbeResponse response;
        CountingClosure done;
        FailingProbeService service;
        InProcessServer server(&service);
        InProcessConnection connection;
        connection.set_server(&server);
        server.set_client(&connection);
        raft::rdma::RdmaControlService::Stub stub(&connection);

        stub.Probe(&controller, &request, &response, &done);

        EXPECT_EQ(done.runs.load(), 1);
        EXPECT_TRUE(controller.Failed());
        EXPECT_EQ(controller.ErrorText(), "service failure");
        EXPECT_EQ(server.responses(), 1);
        EXPECT_EQ(server.pending_request_count(), 0u);
        EXPECT_EQ(connection.pending_rpc_count(), 0u);
    }

    TEST(RdmaTransportContractTest, InProcessLateResponseForClosedClientIsDropped)
    {
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(88);
        raft::rdma::ProbeResponse response;
        CountingClosure done;
        DeferredProbeService service;
        InProcessServer server(&service);
        InProcessConnection connection;
        connection.set_server(&server);
        server.set_client(&connection);
        raft::rdma::RdmaControlService::Stub stub(&connection);

        stub.Probe(&controller, &request, &response, &done);
        ASSERT_TRUE(service.WaitForPending(1, kAsyncTimeout));

        connection.Close();
        EXPECT_EQ(done.runs.load(), 1);
        EXPECT_TRUE(controller.Failed());

        ASSERT_TRUE(service.Complete(88, "too late"));

        EXPECT_EQ(server.responses(), 1);
        EXPECT_EQ(server.pending_request_count(), 0u);
        EXPECT_EQ(connection.pending_rpc_count(), 0u);
        EXPECT_EQ(done.runs.load(), 1);
    }

    TEST(RdmaTransportContractTest, InProcessTransportFailureSkipsResponseSend)
    {
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(99);
        raft::rdma::ProbeResponse response;
        CountingClosure done;
        DeferredProbeService service;
        InProcessServer server(&service);
        InProcessConnection connection;
        connection.set_server(&server);
        server.set_client(&connection);
        raft::rdma::RdmaControlService::Stub stub(&connection);

        stub.Probe(&controller, &request, &response, &done);
        ASSERT_TRUE(service.WaitForPending(1, kAsyncTimeout));

        server.OnTransportFailure("client connection failed");
        ASSERT_TRUE(service.Complete(99, "after-failure"));

        EXPECT_EQ(server.responses(), 0);
        EXPECT_EQ(server.pending_request_count(), 0u);
        EXPECT_EQ(done.runs.load(), 0);
    }

    TEST(RdmaTransportContractTest, InProcessStopWaitsForPendingServiceCompletion)
    {
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        request.set_probe_id(11);
        raft::rdma::ProbeResponse response;
        CountingClosure done;
        DeferredProbeService service;
        InProcessServer server(&service);
        InProcessConnection connection;
        connection.set_server(&server);
        server.set_client(&connection);
        raft::rdma::RdmaControlService::Stub stub(&connection);

        stub.Probe(&controller, &request, &response, &done);
        ASSERT_TRUE(service.WaitForPending(1, kAsyncTimeout));

        std::atomic<bool> stop_returned{false};
        std::thread stopper([&server, &stop_returned]
                            {
                                server.Stop();
                                stop_returned.store(true);
                            });

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT_FALSE(stop_returned.load());

        ASSERT_TRUE(service.Complete(11, "after-stop"));
        stopper.join();

        EXPECT_TRUE(stop_returned.load());
        EXPECT_EQ(server.responses(), 0);
        EXPECT_EQ(server.pending_request_count(), 0u);
        EXPECT_EQ(done.runs.load(), 0);
    }

    enum class PullStoreOutcome
    {
        kDurableSuccess,
        kAlreadyExists,
        kConflict,
        kChecksumMismatch,
        kIoError,
    };

    class PullSimulationStore
    {
    public:
        void set_outcome(PullStoreOutcome outcome)
        {
            outcome_ = outcome;
        }

        PullStoreOutcome outcome() const
        {
            return outcome_;
        }

        void BeginWrite()
        {
            ++write_calls_;
            write_in_progress_ = true;
        }

        void FinishWrite()
        {
            write_in_progress_ = false;
            ++completed_writes_;
        }

        std::size_t write_calls() const
        {
            return write_calls_;
        }

        std::size_t completed_writes() const
        {
            return completed_writes_;
        }

        bool write_in_progress() const
        {
            return write_in_progress_;
        }

    private:
        PullStoreOutcome outcome_ = PullStoreOutcome::kDurableSuccess;
        std::size_t write_calls_ = 0;
        std::size_t completed_writes_ = 0;
        bool write_in_progress_ = false;
    };

    class PullSimulationService final : public raft::rdma::RdmaControlService
    {
    public:
        explicit PullSimulationService(PullSimulationStore& store)
            : store_(store)
        {
        }

        void Probe(google::protobuf::RpcController* controller,
                   const raft::rdma::ProbeRequest* request,
                   raft::rdma::ProbeResponse* response,
                   google::protobuf::Closure* done) override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            controller_ = controller;
            response_ = response;
            done_ = done;
            identity_ = request->probe_id();
            read_posted_ = true;
            read_completed_ = false;
            cv_.notify_all();
        }

        bool WaitForReadPosted(std::chrono::milliseconds timeout)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            return cv_.wait_for(lock, timeout, [this] { return read_posted_; });
        }

        void SimulateReadCompletion()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            read_completed_ = true;
        }

        bool read_completed() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return read_completed_;
        }

        bool store_pending() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return store_pending_;
        }

        bool BeginStoreWrite()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!read_completed_ || store_pending_)
            {
                return false;
            }
            store_.BeginWrite();
            store_pending_ = true;
            cv_.notify_all();
            return true;
        }

        bool FinishStoreWrite()
        {
            google::protobuf::RpcController* controller = nullptr;
            raft::rdma::ProbeResponse* response = nullptr;
            google::protobuf::Closure* done = nullptr;
            std::uint64_t identity = 0;
            PullStoreOutcome outcome = PullStoreOutcome::kDurableSuccess;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!store_pending_)
                {
                    return false;
                }
                controller = controller_;
                response = response_;
                done = done_;
                identity = identity_;
                outcome = store_.outcome();
                store_.FinishWrite();
                store_pending_ = false;
            }

            switch (outcome)
            {
            case PullStoreOutcome::kDurableSuccess:
            case PullStoreOutcome::kAlreadyExists:
                response->set_probe_id(identity);
                response->set_ok(true);
                response->set_message(outcome == PullStoreOutcome::kAlreadyExists ? "durable-already-exists"
                                                                                  : "durable-success");
                break;
            case PullStoreOutcome::kConflict:
                controller->SetFailed("chunk store conflict");
                break;
            case PullStoreOutcome::kChecksumMismatch:
                controller->SetFailed("chunk checksum mismatch");
                break;
            case PullStoreOutcome::kIoError:
                controller->SetFailed("chunk store io error");
                break;
            }

            if (done != nullptr)
            {
                done->Run();
            }
            return true;
        }

    private:
        PullSimulationStore& store_;
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        google::protobuf::RpcController* controller_ = nullptr;
        raft::rdma::ProbeResponse* response_ = nullptr;
        google::protobuf::Closure* done_ = nullptr;
        std::uint64_t identity_ = 0;
        bool read_posted_ = false;
        bool read_completed_ = false;
        bool store_pending_ = false;
    };

    struct PullSimulationFixture
    {
        PullSimulationStore store;
        PullSimulationService service{store};
        TestRpcController controller;
        raft::rdma::ProbeRequest request;
        raft::rdma::ProbeResponse response;
        CountingClosure done;
        InProcessServer server{&service};
        InProcessConnection connection;

        void Wire()
        {
            connection.set_server(&server);
            server.set_client(&connection);
        }

        void DrainPendingRead()
        {
            service.SimulateReadCompletion();
            service.BeginStoreWrite();
            service.FinishStoreWrite();
        }
    };

    TEST(RdmaTransportContractTest, PullSimulationControlCallReturnsBeforeReadCompletion)
    {
        PullSimulationFixture fixture;
        fixture.Wire();
        fixture.request.set_probe_id(9001);
        raft::rdma::RdmaControlService::Stub stub(&fixture.connection);

        stub.Probe(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);

        ASSERT_TRUE(fixture.service.WaitForReadPosted(kAsyncTimeout));
        EXPECT_FALSE(fixture.service.read_completed());
        EXPECT_EQ(fixture.done.runs.load(), 0);
        EXPECT_EQ(fixture.server.responses(), 0);
        EXPECT_EQ(fixture.server.pending_request_count(), 1u);
        EXPECT_EQ(fixture.connection.pending_rpc_count(), 1u);
        EXPECT_FALSE(fixture.controller.Failed());

        fixture.DrainPendingRead();
        EXPECT_EQ(fixture.done.runs.load(), 1);
    }

    TEST(RdmaTransportContractTest, PullSimulationReadCompletionAloneDoesNotEmitDurableAck)
    {
        PullSimulationFixture fixture;
        fixture.Wire();
        fixture.request.set_probe_id(9002);
        raft::rdma::RdmaControlService::Stub stub(&fixture.connection);

        stub.Probe(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);
        ASSERT_TRUE(fixture.service.WaitForReadPosted(kAsyncTimeout));

        fixture.service.SimulateReadCompletion();

        EXPECT_TRUE(fixture.service.read_completed());
        EXPECT_EQ(fixture.store.write_calls(), 0u);
        EXPECT_EQ(fixture.server.responses(), 0);
        EXPECT_EQ(fixture.done.runs.load(), 0);
        EXPECT_FALSE(fixture.controller.Failed());
        EXPECT_FALSE(fixture.response.ok());

        fixture.DrainPendingRead();
        EXPECT_EQ(fixture.done.runs.load(), 1);
    }

    TEST(RdmaTransportContractTest, PullSimulationNoAckWhileStoreWriteIsInFlight)
    {
        PullSimulationFixture fixture;
        fixture.Wire();
        fixture.request.set_probe_id(9003);
        raft::rdma::RdmaControlService::Stub stub(&fixture.connection);

        stub.Probe(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);
        ASSERT_TRUE(fixture.service.WaitForReadPosted(kAsyncTimeout));
        fixture.service.SimulateReadCompletion();
        ASSERT_TRUE(fixture.service.BeginStoreWrite());

        EXPECT_TRUE(fixture.store.write_in_progress());
        EXPECT_EQ(fixture.store.write_calls(), 1u);
        EXPECT_EQ(fixture.store.completed_writes(), 0u);
        EXPECT_EQ(fixture.server.responses(), 0);
        EXPECT_EQ(fixture.done.runs.load(), 0);
        EXPECT_FALSE(fixture.controller.Failed());

        ASSERT_TRUE(fixture.service.FinishStoreWrite());
        EXPECT_EQ(fixture.done.runs.load(), 1);
    }

    TEST(RdmaTransportContractTest, PullSimulationDurableAckFollowsStoreWriteSuccess)
    {
        PullSimulationFixture fixture;
        fixture.Wire();
        fixture.store.set_outcome(PullStoreOutcome::kDurableSuccess);
        fixture.request.set_probe_id(9004);
        raft::rdma::RdmaControlService::Stub stub(&fixture.connection);

        stub.Probe(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);
        ASSERT_TRUE(fixture.service.WaitForReadPosted(kAsyncTimeout));
        fixture.service.SimulateReadCompletion();
        ASSERT_TRUE(fixture.service.BeginStoreWrite());
        ASSERT_TRUE(fixture.service.FinishStoreWrite());

        EXPECT_EQ(fixture.store.completed_writes(), 1u);
        EXPECT_EQ(fixture.server.responses(), 1);
        EXPECT_EQ(fixture.done.runs.load(), 1);
        EXPECT_FALSE(fixture.controller.Failed());
        EXPECT_EQ(fixture.response.probe_id(), 9004u);
        EXPECT_TRUE(fixture.response.ok());
        EXPECT_EQ(fixture.response.message(), "durable-success");
        EXPECT_EQ(fixture.server.pending_request_count(), 0u);
        EXPECT_EQ(fixture.connection.pending_rpc_count(), 0u);
    }

    TEST(RdmaTransportContractTest, PullSimulationStoreFailureDoesNotEmitDurableSuccess)
    {
        PullSimulationFixture fixture;
        fixture.Wire();
        fixture.store.set_outcome(PullStoreOutcome::kIoError);
        fixture.request.set_probe_id(9005);
        raft::rdma::RdmaControlService::Stub stub(&fixture.connection);

        stub.Probe(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);
        ASSERT_TRUE(fixture.service.WaitForReadPosted(kAsyncTimeout));
        fixture.service.SimulateReadCompletion();
        ASSERT_TRUE(fixture.service.BeginStoreWrite());
        ASSERT_TRUE(fixture.service.FinishStoreWrite());

        EXPECT_EQ(fixture.done.runs.load(), 1);
        EXPECT_TRUE(fixture.controller.Failed());
        EXPECT_EQ(fixture.controller.ErrorText(), "chunk store io error");
        EXPECT_FALSE(fixture.response.ok());
        EXPECT_EQ(fixture.server.responses(), 1);
        EXPECT_EQ(fixture.server.pending_request_count(), 0u);
        EXPECT_EQ(fixture.connection.pending_rpc_count(), 0u);
    }

    TEST(RdmaTransportContractTest, PullSimulationChecksumMismatchFailsWithoutPublishingChunk)
    {
        PullSimulationFixture fixture;
        fixture.Wire();
        fixture.store.set_outcome(PullStoreOutcome::kChecksumMismatch);
        fixture.request.set_probe_id(9006);
        raft::rdma::RdmaControlService::Stub stub(&fixture.connection);

        stub.Probe(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);
        ASSERT_TRUE(fixture.service.WaitForReadPosted(kAsyncTimeout));
        fixture.service.SimulateReadCompletion();
        EXPECT_TRUE(fixture.service.read_completed());
        ASSERT_TRUE(fixture.service.BeginStoreWrite());
        ASSERT_TRUE(fixture.service.FinishStoreWrite());

        EXPECT_EQ(fixture.done.runs.load(), 1);
        EXPECT_TRUE(fixture.controller.Failed());
        EXPECT_EQ(fixture.controller.ErrorText(), "chunk checksum mismatch");
        EXPECT_FALSE(fixture.response.ok());
        EXPECT_EQ(fixture.connection.pending_rpc_count(), 0u);
    }

    TEST(RdmaTransportContractTest, PullSimulationIdempotentDuplicateMapsToDurableSuccess)
    {
        PullSimulationFixture fixture;
        fixture.Wire();
        fixture.store.set_outcome(PullStoreOutcome::kAlreadyExists);
        fixture.request.set_probe_id(9007);
        raft::rdma::RdmaControlService::Stub stub(&fixture.connection);

        stub.Probe(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);
        ASSERT_TRUE(fixture.service.WaitForReadPosted(kAsyncTimeout));
        fixture.service.SimulateReadCompletion();
        ASSERT_TRUE(fixture.service.BeginStoreWrite());
        ASSERT_TRUE(fixture.service.FinishStoreWrite());

        EXPECT_EQ(fixture.store.write_calls(), 1u);
        EXPECT_EQ(fixture.done.runs.load(), 1);
        EXPECT_FALSE(fixture.controller.Failed());
        EXPECT_TRUE(fixture.response.ok());
        EXPECT_EQ(fixture.response.message(), "durable-already-exists");
    }

    TEST(RdmaTransportContractTest, PullSimulationConflictMapsToFailure)
    {
        PullSimulationFixture fixture;
        fixture.Wire();
        fixture.store.set_outcome(PullStoreOutcome::kConflict);
        fixture.request.set_probe_id(9008);
        raft::rdma::RdmaControlService::Stub stub(&fixture.connection);

        stub.Probe(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);
        ASSERT_TRUE(fixture.service.WaitForReadPosted(kAsyncTimeout));
        fixture.service.SimulateReadCompletion();
        ASSERT_TRUE(fixture.service.BeginStoreWrite());
        ASSERT_TRUE(fixture.service.FinishStoreWrite());

        EXPECT_EQ(fixture.done.runs.load(), 1);
        EXPECT_TRUE(fixture.controller.Failed());
        EXPECT_EQ(fixture.controller.ErrorText(), "chunk store conflict");
        EXPECT_FALSE(fixture.response.ok());
    }

    TEST(RdmaTransportContractTest, PullSimulationClientCloseDuringReadDropsLateResponse)
    {
        PullSimulationFixture fixture;
        fixture.Wire();
        fixture.request.set_probe_id(9009);
        raft::rdma::RdmaControlService::Stub stub(&fixture.connection);

        stub.Probe(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);
        ASSERT_TRUE(fixture.service.WaitForReadPosted(kAsyncTimeout));

        fixture.connection.Close();
        EXPECT_EQ(fixture.done.runs.load(), 1);
        EXPECT_TRUE(fixture.controller.Failed());
        EXPECT_EQ(fixture.connection.pending_rpc_count(), 0u);

        fixture.service.SimulateReadCompletion();
        ASSERT_TRUE(fixture.service.BeginStoreWrite());
        ASSERT_TRUE(fixture.service.FinishStoreWrite());

        EXPECT_EQ(fixture.server.responses(), 1);
        EXPECT_EQ(fixture.done.runs.load(), 1);
        EXPECT_FALSE(fixture.response.ok());
        EXPECT_EQ(fixture.server.pending_request_count(), 0u);
    }

    TEST(RdmaTransportContractTest, PullSimulationServerTransportFailureSkipsAckButReleasesContext)
    {
        PullSimulationFixture fixture;
        fixture.Wire();
        fixture.request.set_probe_id(9010);
        raft::rdma::RdmaControlService::Stub stub(&fixture.connection);

        stub.Probe(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);
        ASSERT_TRUE(fixture.service.WaitForReadPosted(kAsyncTimeout));
        fixture.service.SimulateReadCompletion();
        ASSERT_TRUE(fixture.service.BeginStoreWrite());

        fixture.server.OnTransportFailure("connection lost during pull");
        ASSERT_TRUE(fixture.service.FinishStoreWrite());

        EXPECT_EQ(fixture.server.responses(), 0);
        EXPECT_EQ(fixture.server.pending_request_count(), 0u);
        EXPECT_EQ(fixture.done.runs.load(), 0);

        fixture.connection.Close();
        EXPECT_EQ(fixture.done.runs.load(), 1);
        EXPECT_TRUE(fixture.controller.Failed());
        EXPECT_EQ(fixture.connection.pending_rpc_count(), 0u);
    }

    TEST(RdmaTransportContractTest, PullAsyncRdmaReadCompletionAwaitsT009T010)
    {
        GTEST_SKIP() << "requires T009 RdmaMemoryRegion and T010 StorageRdmaService: "
                        "server must post an asynchronous RDMA READ and return from the Pull method "
                        "before the READ completion is delivered";
    }

    TEST(RdmaTransportContractTest, PullControlMessageSeparatesDescriptorFromChunkPayload)
    {
        raft::rdma::PullRequest request;
        request.set_chunk_id("object-1~7~0");
        request.set_offset(0);
        request.set_length(128ULL * 1024ULL * 1024ULL);
        request.set_remote_address(0x00007F0012345000ULL);
        request.set_remote_rkey(0x00ABCDEFu);
        request.set_descriptor_generation(42);
        request.mutable_checksum()->set_algorithm(raft::rdma::RDMA_CHUNK_CHECKSUM_ALGORITHM_SHA256);
        request.mutable_checksum()->set_value(std::string(64, 'a'));
        request.mutable_checksum()->set_size_bytes(128ULL * 1024ULL * 1024ULL);

        std::string bytes;
        ASSERT_TRUE(request.SerializeToString(&bytes));
        EXPECT_LT(bytes.size(), 512u);

        raft::rdma::PullRequest parsed;
        ASSERT_TRUE(parsed.ParseFromString(bytes));
        EXPECT_EQ(parsed.chunk_id(), "object-1~7~0");
        EXPECT_EQ(parsed.offset(), 0u);
        EXPECT_EQ(parsed.length(), 128ULL * 1024ULL * 1024ULL);
        EXPECT_EQ(parsed.remote_address(), 0x00007F0012345000ULL);
        EXPECT_EQ(parsed.remote_rkey(), 0x00ABCDEFu);
        EXPECT_EQ(parsed.descriptor_generation(), 42u);
        EXPECT_EQ(parsed.checksum().algorithm(), raft::rdma::RDMA_CHUNK_CHECKSUM_ALGORITHM_SHA256);
        EXPECT_EQ(parsed.checksum().value().size(), 64u);
        EXPECT_EQ(parsed.checksum().size_bytes(), 128ULL * 1024ULL * 1024ULL);
    }

    TEST(RdmaTransportContractTest, PullClientMemoryRegionLifetimeAwaitsT009)
    {
        GTEST_SKIP() << "requires T009 RdmaMemoryRegion: the client region must stay registered until "
                        "the final response proves the server READ finished, and a failed RPC alone "
                        "must not allow early deregistration";
    }

    TEST(RdmaTransportContractTest, PullServerMemoryRegionLifetimeAwaitsT009T010)
    {
        GTEST_SKIP() << "requires T009 RdmaMemoryRegion and T010 StorageRdmaService: the server target "
                        "region must stay registered through READ completion, checksum verification, "
                        "and ChunkStore use, with exactly one deregister/free on the final path";
    }

    TEST(RdmaTransportContractTest, PullDisconnectBeforeReadPostedAwaitsT005T009T010)
    {
        GTEST_SKIP() << "requires T005 transport plus T009/T010: disconnecting before the READ is posted "
                        "must release the pull context without any remote-side effect";
    }

    TEST(RdmaTransportContractTest, PullDisconnectAfterReadPostedAwaitsT005T009T010)
    {
        GTEST_SKIP() << "requires T005 transport plus T009/T010: an in-flight READ must be drained or "
                        "reconciled safely, with no use-after-free and no duplicate completion callback";
    }

    TEST(RdmaTransportContractTest, PullDisconnectAfterReadCompletionAwaitsT010)
    {
        GTEST_SKIP() << "requires T010 StorageRdmaService: when the connection drops after READ completion "
                        "but before the durable boundary, the result must not be reported as durable success";
    }

    TEST(RdmaTransportContractTest, PullRealChecksumVerificationAwaitsT010)
    {
        GTEST_SKIP() << "requires T010 storage adapter with the real chunk checksum contract: mismatched "
                        "bytes must never be published as a valid chunk or reported as durable success";
    }

    TEST(RdmaTransportContractTest, PullRealIdempotencyAgainstChunkStoreAwaitsT010)
    {
        GTEST_SKIP() << "requires T010 mapping to ChunkStore::WriteChunk(): duplicate identities must follow "
                        "the existing idempotent-duplicate or conflict rules without redefining them";
    }

    TEST(RdmaTransportContractTest, PullRealDurableAckOrderingAwaitsT010)
    {
        GTEST_SKIP() << "requires T010 StorageRdmaService: durable success may be emitted only after "
                        "ChunkStore::WriteChunk() reports durable success, never at RDMA READ completion";
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
        if (!loopback.Start(&service, &error))
        {
            GTEST_SKIP() << "RDMA transport unavailable: " << error;
        }

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
        if (!loopback.Start(&service, &error))
        {
            GTEST_SKIP() << "RDMA transport unavailable: " << error;
        }

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
        if (!loopback.Start(&service, &error))
        {
            GTEST_SKIP() << "RDMA transport unavailable: " << error;
        }

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
        if (!loopback.Start(&service, &error))
        {
            GTEST_SKIP() << "RDMA transport unavailable: " << error;
        }

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
        if (!loopback.Start(&service, &error))
        {
            GTEST_SKIP() << "RDMA transport unavailable: " << error;
        }

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
        if (!loopback.Start(&service, &error))
        {
            GTEST_SKIP() << "RDMA transport unavailable: " << error;
        }

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
        if (!loopback.Start(&service, &error))
        {
            GTEST_SKIP() << "RDMA transport unavailable: " << error;
        }

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
        if (!loopback.Start(&service, &error))
        {
            GTEST_SKIP() << "RDMA transport unavailable: " << error;
        }

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
        if (!loopback.Start(&service, &error))
        {
            GTEST_SKIP() << "RDMA transport unavailable: " << error;
        }

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
        if (!loopback.Start(&service, &error))
        {
            GTEST_SKIP() << "RDMA transport unavailable: " << error;
        }

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

#if defined(CQUPT_RDMA_CONTRACT_HAS_STORAGE_RDMA_SERVICE)

    // 生成合法 chunk_id（object-1 / version 7 / chunk_index 0）。
    std::string MakeValidPullChunkId()
    {
        std::string chunk_id;
        storedemo::MakeChunkId("object-1", 7, 0, &chunk_id, nullptr);
        return chunk_id;
    }

    // 用现有 store 算法计算 SHA256 并映射成 proto checksum。
    raft::rdma::RdmaChunkChecksum MakePullChecksum(const std::string& payload)
    {
        storedemo::ChunkChecksum checksum;
        storedemo::ComputeChunkChecksum(payload, &checksum, nullptr);

        raft::rdma::RdmaChunkChecksum proto;
        proto.set_algorithm(raft::rdma::RDMA_CHUNK_CHECKSUM_ALGORITHM_SHA256);
        proto.set_value(checksum.value);
        proto.set_size_bytes(checksum.size_bytes);
        return proto;
    }

    // 构造 kOk + durable 的 WriteChunkResponse。
    storedemo::WriteChunkResponse MakeDurableWriteResponse(const std::string& chunk_id)
    {
        storedemo::WriteChunkResponse response;
        response.status = storedemo::StorageNodeStatusCode::kOk;
        response.durable = true;
        response.metadata.identity.chunk_id = chunk_id;
        return response;
    }

    // 记录完成顺序（"ack"）并支持带超时等待的 Closure。
    class OrderingClosure final : public google::protobuf::Closure
    {
    public:
        explicit OrderingClosure(std::shared_ptr<std::vector<std::string>> order)
            : order_(std::move(order))
        {
        }

        // 记录回调发生并唤醒等待者。
        void Run() override
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++runs_;
                if (order_ != nullptr)
                {
                    order_->push_back("ack");
                }
            }
            cv_.notify_all();
        }

        // 等待回调至少执行 expected 次。
        bool WaitForRuns(int expected, std::chrono::milliseconds timeout)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            return cv_.wait_for(lock, timeout, [this, expected] { return runs_ >= expected; });
        }

        // 返回当前回调次数。
        int runs() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return runs_;
        }

    private:
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::shared_ptr<std::vector<std::string>> order_;
        int runs_ = 0;
    };

    // ChunkStore 测试替身：记录 WriteChunk 请求/次数，返回可配置结果并写顺序日志。
    class RecordingChunkStore final : public storedemo::ChunkStore
    {
    public:
        explicit RecordingChunkStore(std::shared_ptr<std::vector<std::string>> order)
            : order_(std::move(order))
        {
        }

        // 记录一次 WriteChunk 调用，返回 set_response 配置的结果。
        storedemo::WriteChunkResponse WriteChunk(const storedemo::WriteChunkRequest& request) override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++write_calls_;
            last_request_ = request;
            if (order_ != nullptr)
            {
                order_->push_back("store");
            }
            return response_;
        }

        // Pull 测试不使用读取接口，返回默认结果。
        storedemo::ReadChunkResponse ReadChunk(const storedemo::ReadChunkRequest&) override
        {
            return {};
        }

        // Pull 测试不使用删除接口，返回默认结果。
        storedemo::DeleteChunkResponse DeleteChunk(const storedemo::DeleteChunkRequest&) override
        {
            return {};
        }

        // Pull 测试不使用 stat 接口，返回默认结果。
        storedemo::StatChunkResponse StatChunk(const storedemo::StatChunkRequest&) override
        {
            return {};
        }

        // Pull 测试不使用 list 接口，返回默认结果。
        storedemo::ListChunksResponse ListChunks(const storedemo::ListChunksRequest&) override
        {
            return {};
        }

        // 设置下一次 WriteChunk 返回的响应。
        void set_response(storedemo::WriteChunkResponse response)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            response_ = std::move(response);
        }

        // 返回 WriteChunk 调用次数。
        std::size_t write_calls() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return write_calls_;
        }

        // 返回最近一次 WriteChunk 收到的请求。
        storedemo::WriteChunkRequest last_request() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return last_request_;
        }

    private:
        mutable std::mutex mutex_;
        std::shared_ptr<std::vector<std::string>> order_;
        storedemo::WriteChunkResponse response_;
        storedemo::WriteChunkRequest last_request_;
        std::size_t write_calls_ = 0;
    };

    // READ 后端测试替身：真实 malloc 目标区（不注册），捕获请求与完成回调供测试手动驱动。
    class FakeStorageRdmaReadBackend final : public storedemo::StorageRdmaReadBackend
    {
    public:
        // 分配目标区；可通过 SetCreateFailure 配置失败路径。
        std::unique_ptr<rdma::RdmaMemoryRegion> CreateTargetRegion(std::size_t size,
                                                                   std::string* error) override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++create_calls_;
            if (!create_ok_)
            {
                if (error != nullptr)
                {
                    *error = create_error_;
                }
                return nullptr;
            }
            return rdma::RdmaMemoryRegion::Allocate(size, error);
        }

        // 捕获 READ 请求与完成回调后立即返回；可通过 SetSubmitFailure 配置失败路径。
        bool SubmitRead(rdma::RdmaMemoryRegion& target,
                        const raft::rdma::PullRequest& request,
                        ReadCompletion completion,
                        std::string* error) override
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++submit_calls_;
                target_ = &target;
                request_ = request;
                completion_ = std::move(completion);
                if (!submit_ok_)
                {
                    if (error != nullptr)
                    {
                        *error = submit_error_;
                    }
                }
                else if (error != nullptr)
                {
                    error->clear();
                }
            }
            cv_.notify_all();
            return submit_ok_;
        }

        // 等待至少一次 SubmitRead 被调用。
        bool WaitForSubmission(std::chrono::milliseconds timeout)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            return cv_.wait_for(lock, timeout, [this] { return submit_calls_ > 0; });
        }

        // 配置 CreateTargetRegion 返回失败及错误文本。
        void SetCreateFailure(std::string error)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            create_ok_ = false;
            create_error_ = std::move(error);
        }

        // 配置 SubmitRead 返回失败及错误文本。
        void SetSubmitFailure(std::string error)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            submit_ok_ = false;
            submit_error_ = std::move(error);
        }

        // 模拟 READ 把字节写入目标区。
        void SetTargetBytes(const std::string& payload)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (target_ != nullptr && target_->data() != nullptr)
            {
                std::memcpy(target_->data(), payload.data(), payload.size());
            }
        }

        // 触发 READ 完成回调；可重复调用以验证 service 的恰好一次保护。
        void CompleteRead(bool ok, std::string error = {})
        {
            ReadCompletion completion;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                completion = completion_;
            }
            if (completion)
            {
                completion(ok, std::move(error));
            }
        }

        // 返回 CreateTargetRegion 调用次数。
        int create_calls() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return create_calls_;
        }

        // 返回 SubmitRead 调用次数。
        int submit_calls() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return submit_calls_;
        }

    private:
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        bool create_ok_ = true;
        std::string create_error_;
        bool submit_ok_ = true;
        std::string submit_error_;
        int create_calls_ = 0;
        int submit_calls_ = 0;
        rdma::RdmaMemoryRegion* target_ = nullptr;
        raft::rdma::PullRequest request_;
        ReadCompletion completion_;
    };

    // Pull 测试夹具：组装 service + 替身，统一维护顺序日志与合法请求。
    struct PullServiceFixture
    {
        explicit PullServiceFixture(storedemo::BoundedStorageExecutor* executor = nullptr)
            : service(std::make_shared<storedemo::StorageRdmaService>(store, &backend, executor))
        {
            request.set_chunk_id(MakeValidPullChunkId());
        }

        // 设置 length / remote_address / remote_rkey / checksum 描述符。
        void SetPayloadDescriptor(const std::string& payload)
        {
            request.set_length(payload.size());
            request.set_remote_address(0x00007F0000100000ULL);
            request.set_remote_rkey(0x1234);
            *request.mutable_checksum() = MakePullChecksum(payload);
        }

        std::shared_ptr<std::vector<std::string>> order = std::make_shared<std::vector<std::string>>();
        std::shared_ptr<RecordingChunkStore> store = std::make_shared<RecordingChunkStore>(order);
        FakeStorageRdmaReadBackend backend;
        TestRpcController controller;
        raft::rdma::PullRequest request;
        raft::rdma::PullResponse response;
        OrderingClosure done{order};
        std::shared_ptr<storedemo::StorageRdmaService> service;
    };

    TEST(RdmaTransportContractTest, PullServiceSubmitsReadAndReturnsBeforeCompletion)
    {
        PullServiceFixture fixture;
        const std::string payload = "hello-pull-chunk";
        fixture.SetPayloadDescriptor(payload);
        fixture.store->set_response(MakeDurableWriteResponse(fixture.request.chunk_id()));

        fixture.service->Pull(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);

        ASSERT_TRUE(fixture.backend.WaitForSubmission(kAsyncTimeout));
        EXPECT_EQ(fixture.done.runs(), 0);
        EXPECT_EQ(fixture.store->write_calls(), 0u);
        EXPECT_FALSE(fixture.controller.Failed());

        fixture.backend.SetTargetBytes(payload);
        fixture.backend.CompleteRead(true);

        EXPECT_EQ(fixture.done.runs(), 1);
        EXPECT_EQ(fixture.store->write_calls(), 1u);
        EXPECT_FALSE(fixture.controller.Failed());
        EXPECT_TRUE(fixture.response.ok());
        EXPECT_TRUE(fixture.response.durable());
        EXPECT_EQ(fixture.response.chunk_id(), fixture.request.chunk_id());
    }

    TEST(RdmaTransportContractTest, PullServiceForwardsIdentityChecksumAndPayloadToChunkStore)
    {
        PullServiceFixture fixture;
        const std::string payload = "chunk-bytes-0123456789";
        fixture.SetPayloadDescriptor(payload);
        fixture.store->set_response(MakeDurableWriteResponse(fixture.request.chunk_id()));

        fixture.service->Pull(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);
        ASSERT_TRUE(fixture.backend.WaitForSubmission(kAsyncTimeout));

        fixture.backend.SetTargetBytes(payload);
        fixture.backend.CompleteRead(true);

        ASSERT_EQ(fixture.store->write_calls(), 1u);
        const storedemo::WriteChunkRequest recorded = fixture.store->last_request();
        EXPECT_FALSE(recorded.request_id.empty());
        EXPECT_EQ(recorded.identity.chunk_id, fixture.request.chunk_id());
        EXPECT_FALSE(recorded.identity.object_id.empty());
        EXPECT_EQ(recorded.identity.version, 7u);
        EXPECT_EQ(recorded.identity.chunk_index, 0u);
        EXPECT_EQ(recorded.identity.offset, 0u);
        ASSERT_TRUE(recorded.expected_size.has_value());
        EXPECT_EQ(*recorded.expected_size, payload.size());
        EXPECT_EQ(recorded.expected_checksum.algorithm, storedemo::ChunkChecksumAlgorithm::kSha256);
        EXPECT_EQ(recorded.expected_checksum.value, MakePullChecksum(payload).value());
        EXPECT_EQ(recorded.payload, payload);

        EXPECT_EQ(fixture.done.runs(), 1);
        ASSERT_EQ(fixture.order->size(), 2u);
        EXPECT_EQ((*fixture.order)[0], "store");
        EXPECT_EQ((*fixture.order)[1], "ack");
    }

    TEST(RdmaTransportContractTest, PullServiceReadFailureSkipsStoreAndFails)
    {
        PullServiceFixture fixture;
        const std::string payload = "read-failure-chunk";
        fixture.SetPayloadDescriptor(payload);

        fixture.service->Pull(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);
        ASSERT_TRUE(fixture.backend.WaitForSubmission(kAsyncTimeout));

        fixture.backend.CompleteRead(false, "simulated READ failure");

        EXPECT_EQ(fixture.done.runs(), 1);
        EXPECT_EQ(fixture.store->write_calls(), 0u);
        EXPECT_TRUE(fixture.controller.Failed());
        EXPECT_EQ(fixture.controller.ErrorText(), "simulated READ failure");
        EXPECT_FALSE(fixture.response.ok());
        EXPECT_FALSE(fixture.response.durable());
    }

    TEST(RdmaTransportContractTest, PullServiceStoreFailureDoesNotReturnDurableAck)
    {
        PullServiceFixture fixture;
        const std::string payload = "store-failure-chunk";
        fixture.SetPayloadDescriptor(payload);

        storedemo::WriteChunkResponse failure;
        failure.status = storedemo::StorageNodeStatusCode::kIoError;
        failure.error_detail = "disk full";
        failure.durable = false;
        fixture.store->set_response(failure);

        fixture.service->Pull(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);
        ASSERT_TRUE(fixture.backend.WaitForSubmission(kAsyncTimeout));
        fixture.backend.SetTargetBytes(payload);
        fixture.backend.CompleteRead(true);

        EXPECT_EQ(fixture.store->write_calls(), 1u);
        EXPECT_EQ(fixture.done.runs(), 1);
        EXPECT_TRUE(fixture.controller.Failed());
        EXPECT_EQ(fixture.controller.ErrorText(), "disk full");
        EXPECT_FALSE(fixture.response.ok());
        EXPECT_FALSE(fixture.response.durable());
    }

    TEST(RdmaTransportContractTest, PullServiceChecksumMismatchIsReportedAsFailure)
    {
        PullServiceFixture fixture;
        const std::string payload = "checksum-mismatch-chunk";
        fixture.SetPayloadDescriptor(payload);

        storedemo::WriteChunkResponse mismatch;
        mismatch.status = storedemo::StorageNodeStatusCode::kChecksumMismatch;
        mismatch.error_detail = "chunk checksum mismatch";
        mismatch.durable = false;
        fixture.store->set_response(mismatch);

        fixture.service->Pull(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);
        ASSERT_TRUE(fixture.backend.WaitForSubmission(kAsyncTimeout));
        fixture.backend.SetTargetBytes("corrupted-bytes");
        fixture.backend.CompleteRead(true);

        EXPECT_EQ(fixture.done.runs(), 1);
        EXPECT_TRUE(fixture.controller.Failed());
        EXPECT_EQ(fixture.controller.ErrorText(), "chunk checksum mismatch");
        EXPECT_FALSE(fixture.response.ok());
        EXPECT_FALSE(fixture.response.durable());
    }

    TEST(RdmaTransportContractTest, PullServiceIdempotentDuplicateReturnsDurableAlreadyExists)
    {
        PullServiceFixture fixture;
        const std::string payload = "duplicate-chunk-bytes";
        fixture.SetPayloadDescriptor(payload);

        storedemo::WriteChunkResponse duplicate;
        duplicate.status = storedemo::StorageNodeStatusCode::kOk;
        duplicate.durable = true;
        duplicate.already_exists = true;
        duplicate.metadata.identity.chunk_id = fixture.request.chunk_id();
        fixture.store->set_response(duplicate);

        fixture.service->Pull(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);
        ASSERT_TRUE(fixture.backend.WaitForSubmission(kAsyncTimeout));
        fixture.backend.SetTargetBytes(payload);
        fixture.backend.CompleteRead(true);

        EXPECT_EQ(fixture.done.runs(), 1);
        EXPECT_FALSE(fixture.controller.Failed());
        EXPECT_TRUE(fixture.response.ok());
        EXPECT_TRUE(fixture.response.durable());
        EXPECT_TRUE(fixture.response.already_exists());
    }

    TEST(RdmaTransportContractTest, PullServiceConflictDoesNotReturnSuccess)
    {
        PullServiceFixture fixture;
        const std::string payload = "conflicting-chunk-bytes";
        fixture.SetPayloadDescriptor(payload);

        storedemo::WriteChunkResponse conflict;
        conflict.status = storedemo::StorageNodeStatusCode::kConflict;
        conflict.error_detail = "chunk identity conflicts with existing content";
        conflict.durable = false;
        fixture.store->set_response(conflict);

        fixture.service->Pull(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);
        ASSERT_TRUE(fixture.backend.WaitForSubmission(kAsyncTimeout));
        fixture.backend.SetTargetBytes(payload);
        fixture.backend.CompleteRead(true);

        EXPECT_EQ(fixture.done.runs(), 1);
        EXPECT_TRUE(fixture.controller.Failed());
        EXPECT_FALSE(fixture.response.ok());
        EXPECT_FALSE(fixture.response.durable());
    }

    TEST(RdmaTransportContractTest, PullServiceDuplicateReadCompletionRunsOnce)
    {
        PullServiceFixture fixture;
        const std::string payload = "double-completion-chunk";
        fixture.SetPayloadDescriptor(payload);
        fixture.store->set_response(MakeDurableWriteResponse(fixture.request.chunk_id()));

        fixture.service->Pull(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);
        ASSERT_TRUE(fixture.backend.WaitForSubmission(kAsyncTimeout));
        fixture.backend.SetTargetBytes(payload);

        fixture.backend.CompleteRead(true);
        fixture.backend.CompleteRead(true);

        EXPECT_EQ(fixture.store->write_calls(), 1u);
        EXPECT_EQ(fixture.done.runs(), 1);
        EXPECT_FALSE(fixture.controller.Failed());
    }

    TEST(RdmaTransportContractTest, PullServiceMissingBackendFailsExplicitly)
    {
        std::shared_ptr<RecordingChunkStore> store =
            std::make_shared<RecordingChunkStore>(std::make_shared<std::vector<std::string>>());
        storedemo::StorageRdmaService service(store, nullptr);

        TestRpcController controller;
        raft::rdma::PullRequest request;
        request.set_chunk_id(MakeValidPullChunkId());
        request.set_length(1);
        request.set_remote_address(0x1000);
        request.set_remote_rkey(1);
        *request.mutable_checksum() = MakePullChecksum("x");
        raft::rdma::PullResponse response;
        CountingClosure done;

        service.Pull(&controller, &request, &response, &done);

        EXPECT_EQ(done.runs.load(), 1);
        EXPECT_TRUE(controller.Failed());
        EXPECT_NE(controller.ErrorText().find("backend"), std::string::npos);
        EXPECT_EQ(store->write_calls(), 0u);
    }

    TEST(RdmaTransportContractTest, PullServiceTargetRegionFailureFailsBeforeRead)
    {
        PullServiceFixture fixture;
        const std::string payload = "no-region-chunk";
        fixture.SetPayloadDescriptor(payload);
        fixture.backend.SetCreateFailure("no registered protection domain");

        fixture.service->Pull(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);

        EXPECT_EQ(fixture.done.runs(), 1);
        EXPECT_TRUE(fixture.controller.Failed());
        EXPECT_EQ(fixture.controller.ErrorText(), "no registered protection domain");
        EXPECT_EQ(fixture.backend.submit_calls(), 0);
        EXPECT_EQ(fixture.store->write_calls(), 0u);
    }

    TEST(RdmaTransportContractTest, PullServiceSubmitFailureFailsBeforeStore)
    {
        PullServiceFixture fixture;
        const std::string payload = "submit-failure-chunk";
        fixture.SetPayloadDescriptor(payload);
        fixture.backend.SetSubmitFailure("ibv_post_send failed");

        fixture.service->Pull(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);

        EXPECT_EQ(fixture.done.runs(), 1);
        EXPECT_TRUE(fixture.controller.Failed());
        EXPECT_EQ(fixture.controller.ErrorText(), "ibv_post_send failed");
        EXPECT_EQ(fixture.store->write_calls(), 0u);
    }

    TEST(RdmaTransportContractTest, PullServiceRejectsInvalidRequestBeforeRead)
    {
        const auto expect_rejected = [](const std::function<void(raft::rdma::PullRequest&)>& mutate)
        {
            PullServiceFixture fixture;
            const std::string payload = "x";
            fixture.SetPayloadDescriptor(payload);
            mutate(fixture.request);

            fixture.service->Pull(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);

            EXPECT_EQ(fixture.done.runs(), 1);
            EXPECT_TRUE(fixture.controller.Failed());
            EXPECT_EQ(fixture.backend.submit_calls(), 0);
            EXPECT_EQ(fixture.store->write_calls(), 0u);
        };

        expect_rejected([](raft::rdma::PullRequest& request) { request.set_chunk_id(""); });
        expect_rejected([](raft::rdma::PullRequest& request) { request.set_length(0); });
        expect_rejected([](raft::rdma::PullRequest& request)
                        { request.set_length(storedemo::kProductionChunkSizeBytes + 1); });
        expect_rejected([](raft::rdma::PullRequest& request) { request.set_remote_address(0); });
        expect_rejected([](raft::rdma::PullRequest& request) { request.set_remote_rkey(0); });
        expect_rejected([](raft::rdma::PullRequest& request)
                        {
                            request.set_remote_address(std::numeric_limits<std::uint64_t>::max());
                            request.set_length(2);
                        });
        expect_rejected([](raft::rdma::PullRequest& request) { request.clear_checksum(); });
        expect_rejected([](raft::rdma::PullRequest& request) { request.mutable_checksum()->set_value(""); });
    }

    TEST(RdmaTransportContractTest, PullServiceAcceptsMaximumChunkSizeBoundary)
    {
        PullServiceFixture fixture;
        fixture.request.set_chunk_id(MakeValidPullChunkId());
        fixture.request.set_length(storedemo::kProductionChunkSizeBytes);
        fixture.request.set_remote_address(0x1000);
        fixture.request.set_remote_rkey(1);
        fixture.request.mutable_checksum()->set_algorithm(raft::rdma::RDMA_CHUNK_CHECKSUM_ALGORITHM_SHA256);
        fixture.request.mutable_checksum()->set_value(std::string(64, 'a'));
        fixture.request.mutable_checksum()->set_size_bytes(storedemo::kProductionChunkSizeBytes);

        fixture.service->Pull(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);

        EXPECT_EQ(fixture.done.runs(), 0);
        ASSERT_TRUE(fixture.backend.WaitForSubmission(kAsyncTimeout));

        fixture.backend.CompleteRead(false, "test-stop");
        EXPECT_EQ(fixture.done.runs(), 1);
    }

    TEST(RdmaTransportContractTest, PullServiceRunsStorePhaseOnBoundedExecutor)
    {
        storedemo::BoundedStorageExecutor executor({2, 16});
        PullServiceFixture fixture(&executor);
        const std::string payload = "executor-payload";
        fixture.SetPayloadDescriptor(payload);
        fixture.store->set_response(MakeDurableWriteResponse(fixture.request.chunk_id()));

        fixture.service->Pull(&fixture.controller, &fixture.request, &fixture.response, &fixture.done);
        ASSERT_TRUE(fixture.backend.WaitForSubmission(kAsyncTimeout));
        fixture.backend.SetTargetBytes(payload);
        fixture.backend.CompleteRead(true);

        ASSERT_TRUE(fixture.done.WaitForRuns(1, kAsyncTimeout));
        EXPECT_EQ(fixture.store->write_calls(), 1u);
        EXPECT_FALSE(fixture.controller.Failed());
        EXPECT_TRUE(fixture.response.durable());

        const storedemo::StorageExecutorShutdownResult shutdown = executor.Shutdown();
        EXPECT_TRUE(shutdown.stopped);
    }

    TEST(RdmaTransportContractTest, PullServiceEndToEndThroughRdmaServerCompletesAfterStoreWrite)
    {
        PullServiceFixture fixture;
        const std::string payload = "end-to-end-pull";
        fixture.SetPayloadDescriptor(payload);
        fixture.store->set_response(MakeDurableWriteResponse(fixture.request.chunk_id()));

        InProcessServer server(fixture.service.get());
        InProcessConnection connection;
        connection.set_server(&server);
        server.set_client(&connection);

        raft::rdma::RdmaControlService::Stub stub(&connection);
        WaitingClosure done;

        stub.Pull(&fixture.controller, &fixture.request, &fixture.response, &done);
        ASSERT_TRUE(fixture.backend.WaitForSubmission(kAsyncTimeout));
        EXPECT_EQ(done.runs(), 0);
        EXPECT_EQ(fixture.store->write_calls(), 0u);
        EXPECT_EQ(server.responses(), 0);

        fixture.backend.SetTargetBytes(payload);
        fixture.backend.CompleteRead(true);

        ASSERT_TRUE(done.WaitForRuns(1, kAsyncTimeout));
        EXPECT_EQ(fixture.store->write_calls(), 1u);
        EXPECT_FALSE(fixture.controller.Failed());
        EXPECT_TRUE(fixture.response.ok());
        EXPECT_TRUE(fixture.response.durable());
        EXPECT_EQ(server.responses(), 1);
        EXPECT_EQ(server.pending_request_count(), 0u);
        EXPECT_EQ(connection.pending_rpc_count(), 0u);
    }

    TEST(RdmaTransportContractTest, PullServiceClientCloseDuringReadDropsLateAck)
    {
        PullServiceFixture fixture;
        const std::string payload = "client-close-pull";
        fixture.SetPayloadDescriptor(payload);
        fixture.store->set_response(MakeDurableWriteResponse(fixture.request.chunk_id()));

        InProcessServer server(fixture.service.get());
        InProcessConnection connection;
        connection.set_server(&server);
        server.set_client(&connection);

        raft::rdma::RdmaControlService::Stub stub(&connection);
        WaitingClosure done;

        stub.Pull(&fixture.controller, &fixture.request, &fixture.response, &done);
        ASSERT_TRUE(fixture.backend.WaitForSubmission(kAsyncTimeout));

        connection.Close();
        EXPECT_EQ(done.runs(), 1);
        EXPECT_TRUE(fixture.controller.Failed());

        fixture.backend.SetTargetBytes(payload);
        fixture.backend.CompleteRead(true);

        EXPECT_EQ(fixture.store->write_calls(), 1u);
        EXPECT_EQ(server.responses(), 1);
        EXPECT_EQ(done.runs(), 1);
        EXPECT_EQ(server.pending_request_count(), 0u);
        EXPECT_EQ(connection.pending_rpc_count(), 0u);
    }

    TEST(RdmaTransportContractTest, PullServiceServerTransportFailureSkipsAckButFinishesContext)
    {
        PullServiceFixture fixture;
        const std::string payload = "server-failure-pull";
        fixture.SetPayloadDescriptor(payload);
        fixture.store->set_response(MakeDurableWriteResponse(fixture.request.chunk_id()));

        InProcessServer server(fixture.service.get());
        WaitingClosure done;
        InProcessConnection connection;
        connection.set_server(&server);
        server.set_client(&connection);

        raft::rdma::RdmaControlService::Stub stub(&connection);

        stub.Pull(&fixture.controller, &fixture.request, &fixture.response, &done);
        ASSERT_TRUE(fixture.backend.WaitForSubmission(kAsyncTimeout));

        server.OnTransportFailure("client link down");

        fixture.backend.SetTargetBytes(payload);
        fixture.backend.CompleteRead(true);

        EXPECT_EQ(fixture.store->write_calls(), 1u);
        EXPECT_EQ(server.responses(), 0);
        EXPECT_EQ(server.pending_request_count(), 0u);
        EXPECT_EQ(done.runs(), 0);

        connection.Close();
        EXPECT_EQ(done.runs(), 1);
        EXPECT_TRUE(fixture.controller.Failed());
        EXPECT_EQ(connection.pending_rpc_count(), 0u);
    }

#endif

    TEST(RdmaTransportContractTest, PushWritePostReturnsBeforeCompletionAwaitsT013)
    {
        GTEST_SKIP() << "requires T013 asynchronous Push: posting the RDMA WRITE must return before the "
                        "WRITE completion is processed, without assuming the completion runs after the call returns";
    }

    TEST(RdmaTransportContractTest, PushWriteBufferLifetimeAwaitsT013)
    {
        GTEST_SKIP() << "requires T013 and the T009 MR contract: the client WRITE buffer/MR must remain valid "
                        "until the WRITE completion is processed";
    }

    TEST(RdmaTransportContractTest, PushReadyIsPostedFromWriteCompletionOnSameRcQpAwaitsT013)
    {
        GTEST_SKIP() << "requires T013: the WRITE completion callback must post READY as a SEND on the same "
                        "RdmaConnection and RC QP used for the WRITE";
    }

    TEST(RdmaTransportContractTest, PushFailedWriteCompletionDoesNotPostReadyAwaitsT013)
    {
        GTEST_SKIP() << "requires T013: a failed WRITE completion must not post or cause a successful READY";
    }

    TEST(RdmaTransportContractTest, PushReadyCarriesSlotChunkAndGenerationAwaitsT012T013)
    {
        GTEST_SKIP() << "requires T012 READY control messages and T013: READY must identify the slot, chunk "
                        "identity, pool/slot generation, and length for the completed WRITE";
    }

    TEST(RdmaTransportContractTest, PushClientSlotPoolsAreConnectionScopedAwaitsT012)
    {
        GTEST_SKIP() << "requires T012 SlotPool: each Client connection receives descriptors for its own pool only";
    }

    TEST(RdmaTransportContractTest, PushSameSlotIdAcrossPoolsIsNotInterchangeableAwaitsT012T013)
    {
        GTEST_SKIP() << "requires T012/T013: identical slot_id values in two client pools must never be "
                        "interchangeable across connections";
    }

    TEST(RdmaTransportContractTest, PushPoolExhaustionDoesNotAffectOtherClientAwaitsT012)
    {
        GTEST_SKIP() << "requires T012: exhausting Client A’s pool must not change any Client B slot state";
    }

    TEST(RdmaTransportContractTest, PushClientMirrorNeverOverridesStoreAuthorityAwaitsT012T019)
    {
        GTEST_SKIP() << "requires T012 and T019: the client-side slot mirror must not replace store-side "
                        "ownership and generation validation";
    }

    TEST(RdmaTransportContractTest, PushSlotStateMachineFollowsFreeWritingReadyFlushingFreeAwaitsT012)
    {
        GTEST_SKIP() << "requires T012 SlotPool: slots must follow FREE -> WRITING -> READY -> FLUSHING -> FREE";
    }

    TEST(RdmaTransportContractTest, PushIllegalSlotTransitionsAreRejectedAwaitsT012)
    {
        GTEST_SKIP() << "requires T012: WRITING -> FREE before safe completion, FLUSHING -> WRITING, and "
                        "READY -> WRITING must all be rejected";
    }

    TEST(RdmaTransportContractTest, PushBusySlotCannotBeReallocatedAwaitsT012)
    {
        GTEST_SKIP() << "requires T012: a slot in WRITING/READY/FLUSHING must not be selected again";
    }

    TEST(RdmaTransportContractTest, PushStaleConnectionGenerationIsRejectedAwaitsT012)
    {
        GTEST_SKIP() << "requires T012: a READY/descriptor bound to a stale connection generation must be rejected";
    }

    TEST(RdmaTransportContractTest, PushStalePoolGenerationIsRejectedAwaitsT012)
    {
        GTEST_SKIP() << "requires T012: a descriptor from an older pool generation must be rejected after "
                        "a pool refresh/reconnect";
    }

    TEST(RdmaTransportContractTest, PushStaleSlotGenerationIsRejectedAwaitsT012)
    {
        GTEST_SKIP() << "requires T012: a slot whose generation no longer matches the authoritative pool "
                        "generation must be rejected";
    }

    TEST(RdmaTransportContractTest, PushReconnectAndStoreRestartInvalidateOldDescriptorsAwaitsT012)
    {
        GTEST_SKIP() << "requires T012: reconnect and store restart must invalidate old slot descriptors";
    }

    TEST(RdmaTransportContractTest, PushSameSlotIdDifferentGenerationIsRejectedAwaitsT012)
    {
        GTEST_SKIP() << "requires T012: the same slot_id with a different generation must not be treated as "
                        "the same writable slot";
    }

    TEST(RdmaTransportContractTest, PushReadyWithoutWriteIsRejectedAwaitsT013)
    {
        GTEST_SKIP() << "requires T013: READY without a corresponding completed WRITE must be rejected and must "
                        "not persist data";
    }

    TEST(RdmaTransportContractTest, PushReadyForWrongSlotIsRejectedAwaitsT013)
    {
        GTEST_SKIP() << "requires T013: READY naming a slot outside the connection’s pool must be rejected";
    }

    TEST(RdmaTransportContractTest, PushStaleGenerationReadyIsRejectedAwaitsT013)
    {
        GTEST_SKIP() << "requires T013: READY with a stale pool/slot generation must be rejected";
    }

    TEST(RdmaTransportContractTest, PushDuplicateReadyIsRejectedAwaitsT013)
    {
        GTEST_SKIP() << "requires T013: a second READY for the same slot/chunk must not publish or ACK twice";
    }

    TEST(RdmaTransportContractTest, PushReadyExceedingSlotCapacityIsRejectedAwaitsT013)
    {
        GTEST_SKIP() << "requires T013: READY with length greater than the slot capacity must be rejected";
    }

    TEST(RdmaTransportContractTest, PushNoDurableAckBeforeStoreWriteAwaitsT013)
    {
        GTEST_SKIP() << "requires T013: WRITE completion and READY must not produce a durable ACK before "
                        "ChunkStore::WriteChunk() runs";
    }

    TEST(RdmaTransportContractTest, PushStoreFailureReturnsFailureNotDurableAckAwaitsT013)
    {
        GTEST_SKIP() << "requires T013: a failed ChunkStore::WriteChunk() must return failure without durable ACK";
    }

    TEST(RdmaTransportContractTest, PushDurableAckOnlyAfterWriteChunkSuccessAwaitsT013)
    {
        GTEST_SKIP() << "requires T013: durable success may be emitted only after ChunkStore::WriteChunk() "
                        "reports durable success";
    }

    TEST(RdmaTransportContractTest, PushChecksumMismatchAndConflictDoNotPublishAwaitsT013)
    {
        GTEST_SKIP() << "requires T013: checksum mismatch and identity conflict must not be published, while "
                        "compatible duplicates follow the existing WriteChunk idempotent semantics";
    }

    TEST(RdmaTransportContractTest, PushSlotReuseOnlyAfterDurableAckAwaitsT012T013)
    {
        GTEST_SKIP() << "requires T012/T013: a slot may return to FREE and be reused only after the durable "
                        "result and safe completion of DMA and store use";
    }

    TEST(RdmaTransportContractTest, PushSlotReuseRequiresSafeDmaAndStoreCompletionAwaitsT013)
    {
        GTEST_SKIP() << "requires T013: an error path must still wait for DMA and store usage to end before "
                        "declaring the slot reusable";
    }

    TEST(RdmaTransportContractTest, PushSecondWriteDoesNotCorruptFirstChunkAwaitsT012T013)
    {
        GTEST_SKIP() << "requires T012/T013: a second WRITE reusing a slot must not corrupt the first chunk "
                        "already persisted and ACKed";
    }

    TEST(RdmaTransportContractTest, PushRejectsWhenNoFreeSlotBeforeWriteAwaitsT012T013)
    {
        GTEST_SKIP() << "requires T012/T013: temporary exhaustion of the client’s own pool must be rejected "
                        "before posting RDMA WRITE";
    }

    TEST(RdmaTransportContractTest, PushRejectsInsufficientSlotCapacityBeforeWriteAwaitsT012T013)
    {
        GTEST_SKIP() << "requires T012/T013: when no assigned slot has sufficient capacity, the write must be "
                        "rejected before posting RDMA WRITE";
    }

    TEST(RdmaTransportContractTest, PushOversizedChunkIsInvalidNotResourceExhaustionAwaitsT012T013)
    {
        GTEST_SKIP() << "requires T012/T013: a chunk above the system maximum must be invalid input, not "
                        "temporary resource exhaustion";
    }

    TEST(RdmaTransportContractTest, PushPreTransferResourceRejectionMapsToRejectedBeforeTransferAwaitsT013)
    {
        GTEST_SKIP() << "requires T013: a resource rejection before any remote data operation maps to "
                        "rejected_before_transfer and never to success";
    }

    TEST(RdmaTransportContractTest, PushPostWriteUnknownStateMapsToRemoteStateUncertainAwaitsT014)
    {
        GTEST_SKIP() << "requires T013/T014: a connection failure after posting RDMA WRITE maps to "
                        "remote_state_uncertain, not rejected_before_transfer";
    }

    TEST(RdmaTransportContractTest, PushAsyncContextOutlivesWriteReadyAndStorePhasesAwaitsT013)
    {
        GTEST_SKIP() << "requires T013: WRITE, READY, and store contexts must remain valid across their "
                        "asynchronous phases and be released only after the RPC completes";
    }

    TEST(RdmaTransportContractTest, PushCloseDuringReadyDoesNotDoubleCompleteAwaitsT013)
    {
        GTEST_SKIP() << "requires T013: connection close racing with READY/store completion must not run the "
                        "completion closure twice";
    }

    TEST(RdmaTransportContractTest, PushDisconnectInvalidatesSlotsWithoutReuseAwaitsT013)
    {
        GTEST_SKIP() << "requires T013: after disconnect the store must not treat invalidated slots as safely "
                        "reusable without reconciliation";
    }

#endif
}
