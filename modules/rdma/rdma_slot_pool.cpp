#include "rdma/rdma_slot_pool.h"

#include <utility>

namespace rdma
{
    namespace
    {
        void SetError(std::string* error, const std::string& message)
        {
            if (error != nullptr)
            {
                *error = message;
            }
        }

        bool CheckRemoteSlot(RemoteSlotPool& pool,
                             std::uint32_t slot_id,
                             std::uint64_t slot_generation,
                             SlotState expected_state,
                             std::string* error,
                             RemoteSlot** out_slot)
        {
            for (RemoteSlot& slot : pool.slots)
            {
                if (slot.slot_id != slot_id)
                {
                    continue;
                }

                if (slot.generation != slot_generation)
                {
                    SetError(error, "stale slot generation");
                    return false;
                }

                if (slot.state != expected_state)
                {
                    SetError(error, "illegal slot state transition");
                    return false;
                }

                if (out_slot != nullptr)
                {
                    *out_slot = &slot;
                }
                return true;
            }

            SetError(error, "unknown slot id");
            return false;
        }
    }

    ClientSlotPool::ClientSlotPool(SlotPoolOptions options)
        : options_(std::move(options))
    {
    }

    ClientSlotPool::~ClientSlotPool() = default;

    std::unique_ptr<ClientSlotPool> ClientSlotPool::Create(SlotMemoryProvider* provider,
                                                           SlotPoolOptions options,
                                                           std::string* error)
    {
        if (provider == nullptr)
        {
            SetError(error, "RDMA slot memory provider is unavailable (T005 transport pending)");
            return nullptr;
        }

        if (options.slot_count == 0)
        {
            SetError(error, "slot pool requires a non-zero slot count");
            return nullptr;
        }

        if (options.slot_capacity == 0)
        {
            SetError(error, "slot pool requires a non-zero slot capacity");
            return nullptr;
        }

        if (options.connection_generation == 0 || options.pool_generation == 0)
        {
            SetError(error, "slot pool requires non-zero connection and pool generations");
            return nullptr;
        }

        if (options.storage_node_id.empty())
        {
            SetError(error, "slot pool requires a storage node id");
            return nullptr;
        }

        std::unique_ptr<ClientSlotPool> pool(new ClientSlotPool(std::move(options)));
        pool->slots_.reserve(pool->options_.slot_count);

        for (std::size_t index = 0; index < pool->options_.slot_count; ++index)
        {
            std::string detail;
            std::unique_ptr<RdmaMemoryRegion> region =
                provider->AllocateSlotRegion(pool->options_.slot_capacity, &detail);
            if (region == nullptr)
            {
                SetError(error, detail.empty() ? "failed to allocate a slot region" : detail);
                return nullptr;
            }

            SlotEntry entry;
            entry.descriptor.slot_id = static_cast<std::uint32_t>(index);
            entry.descriptor.remote_addr = reinterpret_cast<std::uint64_t>(region->data());
            entry.descriptor.rkey = region->rkey();
            entry.descriptor.capacity = pool->options_.slot_capacity;
            entry.descriptor.generation = pool->options_.pool_generation;
            entry.descriptor.state = SlotState::kFree;
            entry.region = std::move(region);
            pool->slots_.push_back(std::move(entry));
        }

        return pool;
    }

    std::uint64_t ClientSlotPool::connection_generation() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return options_.connection_generation;
    }

    std::uint64_t ClientSlotPool::pool_generation() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return options_.pool_generation;
    }

    std::size_t ClientSlotPool::slot_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return slots_.size();
    }

    std::size_t ClientSlotPool::slot_capacity() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return options_.slot_capacity;
    }

    std::string ClientSlotPool::storage_node_id() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return options_.storage_node_id;
    }

    bool ClientSlotPool::valid() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return valid_;
    }

    std::vector<RemoteSlot> ClientSlotPool::Snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<RemoteSlot> snapshot;
        if (!valid_)
        {
            return snapshot;
        }

        snapshot.reserve(slots_.size());
        for (const SlotEntry& entry : slots_)
        {
            snapshot.push_back(entry.descriptor);
        }
        return snapshot;
    }

    std::optional<RemoteSlot> ClientSlotPool::Slot(std::uint32_t slot_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!valid_)
        {
            return std::nullopt;
        }

        for (const SlotEntry& entry : slots_)
        {
            if (entry.descriptor.slot_id == slot_id)
            {
                return entry.descriptor;
            }
        }
        return std::nullopt;
    }

    const void* ClientSlotPool::slot_data(std::uint32_t slot_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!valid_)
        {
            return nullptr;
        }

        for (const SlotEntry& entry : slots_)
        {
            if (entry.descriptor.slot_id == slot_id)
            {
                return entry.region != nullptr ? entry.region->data() : nullptr;
            }
        }
        return nullptr;
    }

    ClientSlotPool::SlotEntry* ClientSlotPool::FindSlotLocked(std::uint32_t slot_id)
    {
        for (SlotEntry& entry : slots_)
        {
            if (entry.descriptor.slot_id == slot_id)
            {
                return &entry;
            }
        }
        return nullptr;
    }

    bool ClientSlotPool::CheckSlotLocked(std::uint32_t slot_id,
                                         std::uint64_t slot_generation,
                                         SlotState expected_state,
                                         std::string* error,
                                         SlotEntry** out_entry)
    {
        if (!valid_)
        {
            SetError(error, "slot pool is invalidated");
            return false;
        }

        SlotEntry* entry = FindSlotLocked(slot_id);
        if (entry == nullptr)
        {
            SetError(error, "unknown slot id");
            return false;
        }

        if (entry->descriptor.generation != slot_generation ||
            slot_generation != options_.pool_generation)
        {
            SetError(error, "stale slot generation");
            return false;
        }

        if (entry->descriptor.state != expected_state)
        {
            SetError(error, "illegal slot state transition");
            return false;
        }

        if (out_entry != nullptr)
        {
            *out_entry = entry;
        }
        return true;
    }

    bool ClientSlotPool::MarkReady(std::uint32_t slot_id,
                                   std::uint64_t slot_generation,
                                   std::string* error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SlotEntry* entry = nullptr;
        if (!CheckSlotLocked(slot_id, slot_generation, SlotState::kFree, error, &entry))
        {
            return false;
        }

        entry->descriptor.state = SlotState::kReady;
        return true;
    }

    bool ClientSlotPool::BeginFlushing(std::uint32_t slot_id,
                                       std::uint64_t slot_generation,
                                       std::string* error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SlotEntry* entry = nullptr;
        if (!CheckSlotLocked(slot_id, slot_generation, SlotState::kReady, error, &entry))
        {
            return false;
        }

        entry->descriptor.state = SlotState::kFlushing;
        return true;
    }

    bool ClientSlotPool::ReleaseSlot(std::uint32_t slot_id,
                                     std::uint64_t slot_generation,
                                     std::string* error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SlotEntry* entry = nullptr;
        if (!CheckSlotLocked(slot_id, slot_generation, SlotState::kFlushing, error, &entry))
        {
            return false;
        }

        entry->descriptor.state = SlotState::kFree;
        return true;
    }

    bool ClientSlotPool::AbortReady(std::uint32_t slot_id,
                                    std::uint64_t slot_generation,
                                    std::string* error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SlotEntry* entry = nullptr;
        if (!CheckSlotLocked(slot_id, slot_generation, SlotState::kReady, error, &entry))
        {
            return false;
        }

        entry->descriptor.state = SlotState::kFree;
        return true;
    }

    void ClientSlotPool::Invalidate()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        valid_ = false;
    }

    bool RemoteSlotMap::StorePool(RemoteSlotPool pool, std::string* error)
    {
        if (pool.storage_node_id.empty())
        {
            SetError(error, "slot pool requires a storage node id");
            return false;
        }

        if (pool.connection_generation == 0 || pool.pool_generation == 0)
        {
            SetError(error, "slot pool requires non-zero connection and pool generations");
            return false;
        }

        for (const RemoteSlot& slot : pool.slots)
        {
            if (slot.generation != pool.pool_generation)
            {
                SetError(error, "slot generation does not match the pool generation");
                return false;
            }
        }

        const std::string storage_node_id = pool.storage_node_id;
        std::lock_guard<std::mutex> lock(mutex_);
        pools_[storage_node_id] = std::move(pool);
        return true;
    }

    bool RemoteSlotMap::HasPool(const std::string& storage_node_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return pools_.find(storage_node_id) != pools_.end();
    }

    std::optional<RemoteSlotPool> RemoteSlotMap::GetPool(const std::string& storage_node_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = pools_.find(storage_node_id);
        if (entry == pools_.end())
        {
            return std::nullopt;
        }
        return entry->second;
    }

    std::size_t RemoteSlotMap::free_slot_count(const std::string& storage_node_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = pools_.find(storage_node_id);
        if (entry == pools_.end())
        {
            return 0;
        }

        std::size_t count = 0;
        for (const RemoteSlot& slot : entry->second.slots)
        {
            if (slot.state == SlotState::kFree)
            {
                ++count;
            }
        }
        return count;
    }

    std::optional<RemoteSlot> RemoteSlotMap::SelectFreeSlot(const std::string& storage_node_id,
                                                            std::uint64_t required_capacity,
                                                            std::string* error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = pools_.find(storage_node_id);
        if (entry == pools_.end())
        {
            SetError(error, "no slot pool stored for the storage node");
            return std::nullopt;
        }

        RemoteSlot* first_free = nullptr;
        for (RemoteSlot& slot : entry->second.slots)
        {
            if (slot.state != SlotState::kFree)
            {
                continue;
            }

            if (first_free == nullptr)
            {
                first_free = &slot;
            }

            if (slot.capacity >= required_capacity)
            {
                slot.state = SlotState::kWriting;
                return slot;
            }
        }

        if (first_free == nullptr)
        {
            SetError(error, "no free slot available");
        }
        else
        {
            SetError(error, "no free slot with sufficient capacity");
        }
        return std::nullopt;
    }

    bool RemoteSlotMap::MarkReady(const std::string& storage_node_id,
                                  std::uint32_t slot_id,
                                  std::uint64_t slot_generation,
                                  std::string* error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = pools_.find(storage_node_id);
        if (entry == pools_.end())
        {
            SetError(error, "no slot pool stored for the storage node");
            return false;
        }

        RemoteSlot* slot = nullptr;
        if (!CheckRemoteSlot(entry->second, slot_id, slot_generation, SlotState::kWriting, error, &slot))
        {
            return false;
        }

        slot->state = SlotState::kReady;
        return true;
    }

    bool RemoteSlotMap::MarkFlushing(const std::string& storage_node_id,
                                     std::uint32_t slot_id,
                                     std::uint64_t slot_generation,
                                     std::string* error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = pools_.find(storage_node_id);
        if (entry == pools_.end())
        {
            SetError(error, "no slot pool stored for the storage node");
            return false;
        }

        RemoteSlot* slot = nullptr;
        if (!CheckRemoteSlot(entry->second, slot_id, slot_generation, SlotState::kReady, error, &slot))
        {
            return false;
        }

        slot->state = SlotState::kFlushing;
        return true;
    }

    bool RemoteSlotMap::ReleaseSlot(const std::string& storage_node_id,
                                    std::uint32_t slot_id,
                                    std::uint64_t slot_generation,
                                    std::string* error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = pools_.find(storage_node_id);
        if (entry == pools_.end())
        {
            SetError(error, "no slot pool stored for the storage node");
            return false;
        }

        RemoteSlot* slot = nullptr;
        if (!CheckRemoteSlot(entry->second, slot_id, slot_generation, SlotState::kFlushing, error, &slot))
        {
            return false;
        }

        slot->state = SlotState::kFree;
        return true;
    }

    bool RemoteSlotMap::AbortWriting(const std::string& storage_node_id,
                                     std::uint32_t slot_id,
                                     std::uint64_t slot_generation,
                                     std::string* error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = pools_.find(storage_node_id);
        if (entry == pools_.end())
        {
            SetError(error, "no slot pool stored for the storage node");
            return false;
        }

        RemoteSlot* slot = nullptr;
        if (!CheckRemoteSlot(entry->second, slot_id, slot_generation, SlotState::kWriting, error, &slot))
        {
            return false;
        }

        slot->state = SlotState::kFree;
        return true;
    }

    bool RemoteSlotMap::ValidateGenerations(const std::string& storage_node_id,
                                            std::uint64_t expected_connection_generation,
                                            std::uint64_t expected_pool_generation,
                                            std::string* error) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = pools_.find(storage_node_id);
        if (entry == pools_.end())
        {
            SetError(error, "no slot pool stored for the storage node");
            return false;
        }

        if (entry->second.connection_generation != expected_connection_generation)
        {
            SetError(error, "stale connection generation");
            return false;
        }

        if (entry->second.pool_generation != expected_pool_generation)
        {
            SetError(error, "stale pool generation");
            return false;
        }
        return true;
    }

    std::optional<RemoteSlot> RemoteSlotMap::Slot(const std::string& storage_node_id,
                                                  std::uint32_t slot_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = pools_.find(storage_node_id);
        if (entry == pools_.end())
        {
            return std::nullopt;
        }

        for (const RemoteSlot& slot : entry->second.slots)
        {
            if (slot.slot_id == slot_id)
            {
                return slot;
            }
        }
        return std::nullopt;
    }

    void RemoteSlotMap::InvalidatePool(const std::string& storage_node_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pools_.erase(storage_node_id);
    }

    void RemoteSlotMap::InvalidateAll()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pools_.clear();
    }
}
