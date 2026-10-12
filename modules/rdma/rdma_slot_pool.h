#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "rdma/rdma_memory_region.h"

namespace rdma
{
    enum class SlotState : std::uint8_t
    {
        kFree = 0,
        kWriting = 1,
        kReady = 2,
        kFlushing = 3,
    };

    struct RemoteSlot
    {
        std::uint32_t slot_id = 0;
        std::uint64_t remote_addr = 0;
        std::uint32_t rkey = 0;
        std::uint64_t capacity = 0;
        std::uint64_t generation = 0;
        SlotState state = SlotState::kFree;
    };

    // 服务端 Slot MR 分配 seam：真实实现由 T013/T005 使用连接 PD 注册 REMOTE_WRITE 内存。
    class SlotMemoryProvider
    {
    public:
        SlotMemoryProvider() = default;
        SlotMemoryProvider(const SlotMemoryProvider&) = delete;
        SlotMemoryProvider& operator=(const SlotMemoryProvider&) = delete;
        virtual ~SlotMemoryProvider() = default;

        virtual std::unique_ptr<RdmaMemoryRegion> AllocateSlotRegion(std::size_t size, std::string* error) = 0;
    };

    struct SlotPoolOptions
    {
        std::size_t slot_count = 0;
        std::size_t slot_capacity = 0;
        std::uint64_t connection_generation = 0;
        std::uint64_t pool_generation = 0;
        std::string storage_node_id;
    };

    // 服务端权威 SlotPool：每条 Client 连接一个，固定数量、固定容量、长期保留注册内存。
    class ClientSlotPool
    {
    public:
        static std::unique_ptr<ClientSlotPool> Create(SlotMemoryProvider* provider,
                                                      SlotPoolOptions options,
                                                      std::string* error);

        ClientSlotPool(const ClientSlotPool&) = delete;
        ClientSlotPool& operator=(const ClientSlotPool&) = delete;
        ~ClientSlotPool();

        std::uint64_t connection_generation() const;
        std::uint64_t pool_generation() const;
        std::size_t slot_count() const;
        std::size_t slot_capacity() const;
        std::string storage_node_id() const;
        bool valid() const;

        std::vector<RemoteSlot> Snapshot() const;
        std::optional<RemoteSlot> Slot(std::uint32_t slot_id) const;
        const void* slot_data(std::uint32_t slot_id) const;

        bool MarkReady(std::uint32_t slot_id, std::uint64_t slot_generation, std::string* error);
        bool BeginFlushing(std::uint32_t slot_id, std::uint64_t slot_generation, std::string* error);
        bool ReleaseSlot(std::uint32_t slot_id, std::uint64_t slot_generation, std::string* error);
        bool AbortReady(std::uint32_t slot_id, std::uint64_t slot_generation, std::string* error);

        void Invalidate();

    private:
        struct SlotEntry
        {
            RemoteSlot descriptor;
            std::unique_ptr<RdmaMemoryRegion> region;
        };

        explicit ClientSlotPool(SlotPoolOptions options);

        SlotEntry* FindSlotLocked(std::uint32_t slot_id);
        bool CheckSlotLocked(std::uint32_t slot_id,
                             std::uint64_t slot_generation,
                             SlotState expected_state,
                             std::string* error,
                             SlotEntry** out_entry);

        mutable std::mutex mutex_;
        SlotPoolOptions options_;
        std::vector<SlotEntry> slots_;
        bool valid_ = true;
    };

    struct RemoteSlotPool
    {
        std::string storage_node_id;
        std::uint64_t connection_generation = 0;
        std::uint64_t pool_generation = 0;
        std::vector<RemoteSlot> slots;
    };

    // 客户端镜像：只保存服务端返回的描述符，不拥有远端 MR，也不代替服务端权威判定。
    class RemoteSlotMap
    {
    public:
        RemoteSlotMap() = default;
        RemoteSlotMap(const RemoteSlotMap&) = delete;
        RemoteSlotMap& operator=(const RemoteSlotMap&) = delete;
        ~RemoteSlotMap() = default;

        bool StorePool(RemoteSlotPool pool, std::string* error);
        bool HasPool(const std::string& storage_node_id) const;
        std::optional<RemoteSlotPool> GetPool(const std::string& storage_node_id) const;
        std::size_t free_slot_count(const std::string& storage_node_id) const;

        std::optional<RemoteSlot> SelectFreeSlot(const std::string& storage_node_id,
                                                 std::uint64_t required_capacity,
                                                 std::string* error);
        bool MarkReady(const std::string& storage_node_id,
                       std::uint32_t slot_id,
                       std::uint64_t slot_generation,
                       std::string* error);
        bool MarkFlushing(const std::string& storage_node_id,
                          std::uint32_t slot_id,
                          std::uint64_t slot_generation,
                          std::string* error);
        bool ReleaseSlot(const std::string& storage_node_id,
                         std::uint32_t slot_id,
                         std::uint64_t slot_generation,
                         std::string* error);
        bool AbortWriting(const std::string& storage_node_id,
                          std::uint32_t slot_id,
                          std::uint64_t slot_generation,
                          std::string* error);

        bool ValidateGenerations(const std::string& storage_node_id,
                                 std::uint64_t expected_connection_generation,
                                 std::uint64_t expected_pool_generation,
                                 std::string* error) const;

        std::optional<RemoteSlot> Slot(const std::string& storage_node_id, std::uint32_t slot_id) const;

        void InvalidatePool(const std::string& storage_node_id);
        void InvalidateAll();

    private:
        mutable std::mutex mutex_;
        std::unordered_map<std::string, RemoteSlotPool> pools_;
    };
}
