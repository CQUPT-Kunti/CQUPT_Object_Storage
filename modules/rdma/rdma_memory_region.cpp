#include "rdma/rdma_memory_region.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#if defined(CQUPT_RDMA_ENABLED) && CQUPT_RDMA_ENABLED
#include <infiniband/verbs.h>
#endif

namespace rdma
{
    RdmaMemoryRegion::RdmaMemoryRegion(void *buffer, std::size_t size)
        : buffer_(buffer),
          size_(size)
    {
    }

    RdmaMemoryRegion::~RdmaMemoryRegion()
    {
        Release(nullptr);
    }

    std::unique_ptr<RdmaMemoryRegion> RdmaMemoryRegion::Allocate(std::size_t size, std::string *error)
    {
        if (size == 0)
        {
            if (error != nullptr)
            {
                *error = "RdmaMemoryRegion requires a non-zero size";
            }
            return nullptr;
        }

        void *buffer = std::malloc(size);
        if (buffer == nullptr)
        {
            if (error != nullptr)
            {
                *error = "RdmaMemoryRegion: malloc failed";
            }
            return nullptr;
        }

        return std::unique_ptr<RdmaMemoryRegion>(new RdmaMemoryRegion(buffer, size));
    }

    bool RdmaMemoryRegion::Register(ibv_pd *pd, int access_flags, std::string *error)
    {
        if (state_ != RdmaMemoryRegionState::kAllocated)
        {
            if (error != nullptr)
            {
                *error = "RdmaMemoryRegion is not in the allocated state";
            }
            return false;
        }

        if (pd == nullptr)
        {
            if (error != nullptr)
            {
                *error = "RdmaMemoryRegion requires a valid protection domain";
            }
            return false;
        }

        if (access_flags == 0)
        {
            if (error != nullptr)
            {
                *error = "RdmaMemoryRegion requires explicit access flags";
            }
            return false;
        }

#if defined(CQUPT_RDMA_ENABLED) && CQUPT_RDMA_ENABLED
        mr_ = ibv_reg_mr(pd, buffer_, size_, access_flags);
        if (mr_ == nullptr)
        {
            if (error != nullptr)
            {
                *error = "ibv_reg_mr failed: " + std::string(std::strerror(errno));
            }
            return false;
        }
        state_ = RdmaMemoryRegionState::kRegistered;
        return true;
#else
        if (error != nullptr)
        {
            *error = "RDMA backend is unavailable (CQUPT_RDMA=OFF)";
        }
        return false;
#endif
    }

    bool RdmaMemoryRegion::Deregister(std::string *error)
    {
        if (state_ == RdmaMemoryRegionState::kDeregistered || state_ == RdmaMemoryRegionState::kReleased)
        {
            return true;
        }

        if (state_ != RdmaMemoryRegionState::kRegistered || mr_ == nullptr)
        {
            state_ = RdmaMemoryRegionState::kDeregistered;
            return true;
        }

#if defined(CQUPT_RDMA_ENABLED) && CQUPT_RDMA_ENABLED
        if (ibv_dereg_mr(mr_) != 0)
        {
            if (error != nullptr)
            {
                *error = "ibv_dereg_mr failed: " + std::string(std::strerror(errno));
            }
            return false;
        }
#endif

        mr_ = nullptr;
        state_ = RdmaMemoryRegionState::kDeregistered;
        return true;
    }

    bool RdmaMemoryRegion::Release(std::string *error)
    {
        if (state_ == RdmaMemoryRegionState::kReleased)
        {
            return true;
        }

        if (!Deregister(error))
        {
            return false;
        }

        std::free(buffer_);
        buffer_ = nullptr;
        size_ = 0;
        state_ = RdmaMemoryRegionState::kReleased;
        return true;
    }

    void *RdmaMemoryRegion::data() const
    {
        return buffer_;
    }

    std::size_t RdmaMemoryRegion::size() const
    {
        return size_;
    }

    std::uint32_t RdmaMemoryRegion::lkey() const
    {
#if defined(CQUPT_RDMA_ENABLED) && CQUPT_RDMA_ENABLED
        return mr_ != nullptr ? mr_->lkey : 0u;
#else
        return 0u;
#endif
    }

    std::uint32_t RdmaMemoryRegion::rkey() const
    {
#if defined(CQUPT_RDMA_ENABLED) && CQUPT_RDMA_ENABLED
        return mr_ != nullptr ? mr_->rkey : 0u;
#else
        return 0u;
#endif
    }

    ibv_mr *RdmaMemoryRegion::native_handle() const
    {
        return mr_;
    }

    RdmaMemoryRegionState RdmaMemoryRegion::state() const
    {
        return state_;
    }
}
