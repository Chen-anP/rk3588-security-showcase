// dma-heap 分配（第 5c 步）。没有 /dev/dma_heap/system（CI、电脑）就跳过。
#include "media/dma_buffer.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstring>

using rksec::media::DmaBuffer;

TEST(DmaBuffer, AllocateWriteRead)
{
    if (::access("/dev/dma_heap/system", R_OK | W_OK) != 0) {
        GTEST_SKIP() << "没有 /dev/dma_heap/system";
    }
    std::string err;
    const auto b = DmaBuffer::allocate(640 * 640 * 3, err);
    ASSERT_TRUE(b) << err;
    EXPECT_GE(b->fd(), 0);
    EXPECT_EQ(b->size(), 640U * 640 * 3);
    b->begin_cpu_write();
    std::memset(b->data(), 114, b->size());
    b->end_cpu_write();
    b->begin_cpu_read();
    EXPECT_EQ(b->data()[0], 114);
    EXPECT_EQ(b->data()[b->size() - 1], 114);
    b->end_cpu_read();
}

TEST(DmaBuffer, UnknownHeapIsAClearError)
{
    std::string err;
    EXPECT_FALSE(DmaBuffer::allocate(4096, err, "no-such-heap"));
    EXPECT_NE(err.find("/dev/dma_heap/no-such-heap"), std::string::npos) << err;
}
