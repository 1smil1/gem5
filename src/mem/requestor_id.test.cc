#include <gtest/gtest.h>

#include "mem/request.hh"

namespace gem5
{

TEST(RequestorIdentity, LargeEndpointIndicesRoundTrip)
{
    Request request;
    EXPECT_EQ(request.requestorId(), Request::invldRequestorId);
    for (uint32_t id : {0U, 65534U, 65535U, 65536U, 164255U, 1000000U}) {
        request.requestorId(id);
        EXPECT_EQ(request.requestorId(), id);
        EXPECT_NE(request.requestorId(), Request::invldRequestorId);
    }
    EXPECT_GT(Request::invldRequestorId, 1000000U);
}

} // namespace gem5
