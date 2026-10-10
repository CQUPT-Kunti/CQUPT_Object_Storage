#include "rdma/rdma_transport.h"

namespace rdma
{
    RdmaBackendInfo QueryRdmaBackendInfo()
    {
        RdmaBackendInfo info;
        info.kind = RdmaBackendKind::kUnavailable;
        info.available = false;
        info.backend_name = "unavailable";
        info.detail = "RDMA support is disabled (CQUPT_RDMA=OFF)";
        return info;
    }
}
