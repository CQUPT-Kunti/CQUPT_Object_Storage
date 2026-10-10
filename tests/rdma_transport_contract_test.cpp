#include <gtest/gtest.h>

#include "rdma/rdma_transport.h"

namespace
{
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
}
