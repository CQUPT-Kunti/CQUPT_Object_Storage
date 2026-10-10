#include "rdma/rdma_transport.h"

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

namespace rdma
{
    RdmaBackendInfo QueryRdmaBackendInfo()
    {
        RdmaBackendInfo info;
        info.kind = RdmaBackendKind::kLinuxVerbs;
        info.available = true;
        info.backend_name = "librdmacm+libibverbs";
        info.detail = "librdmacm and libibverbs are available; connection flows are not implemented yet";
        return info;
    }
}
