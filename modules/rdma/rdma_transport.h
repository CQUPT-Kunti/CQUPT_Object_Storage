#pragma once

#include <string>

namespace rdma
{
    enum class RdmaBackendKind
    {
        kUnavailable,
        kLinuxVerbs,
    };

    struct RdmaBackendInfo
    {
        RdmaBackendKind kind = RdmaBackendKind::kUnavailable;
        bool available = false;
        std::string backend_name = "unavailable";
        std::string detail;
    };

    RdmaBackendInfo QueryRdmaBackendInfo();
}
