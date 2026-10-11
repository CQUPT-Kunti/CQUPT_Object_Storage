#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

struct ibv_mr;
struct ibv_pd;

namespace rdma
{
    enum class RdmaMemoryRegionState
    {
        kAllocated,
        kRegistered,
        kDeregistered,
        kReleased,
    };

    class RdmaMemoryRegion
    {
    public:
        static std::unique_ptr<RdmaMemoryRegion> Allocate(std::size_t size, std::string* error = nullptr);

        RdmaMemoryRegion(const RdmaMemoryRegion&) = delete;
        RdmaMemoryRegion& operator=(const RdmaMemoryRegion&) = delete;
        ~RdmaMemoryRegion();

        bool Register(ibv_pd* pd, int access_flags, std::string* error = nullptr);
        bool Deregister(std::string* error = nullptr);
        bool Release(std::string* error = nullptr);

        void* data() const;
        std::size_t size() const;
        std::uint32_t lkey() const;
        std::uint32_t rkey() const;
        ibv_mr* native_handle() const;
        RdmaMemoryRegionState state() const;

    private:
        RdmaMemoryRegion(void* buffer, std::size_t size);

        void* buffer_ = nullptr;
        std::size_t size_ = 0;
        ibv_mr* mr_ = nullptr;
        RdmaMemoryRegionState state_ = RdmaMemoryRegionState::kAllocated;
    };
}
