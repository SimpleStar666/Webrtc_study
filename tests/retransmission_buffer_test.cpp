// ============================================================================
// retransmission_buffer_test.cpp — 重传缓冲区单元测试
// ============================================================================
// 覆盖：命中补发字节一致、未命中计数、容量淘汰、TTL 淘汰。
// ============================================================================
#include <gtest/gtest.h>
#include "media/rtcp/retransmission_buffer.h"

using namespace crystal;

static std::vector<uint8_t> makePacket(uint16_t seq, size_t size) {
    std::vector<uint8_t> p(size);
    p[0] = 0x80;
    p[1] = static_cast<uint8_t>(seq & 0xFF);  // 用 seq 标记内容
    return p;
}

TEST(RetransmissionBufferTest, StoreAndGetReturnsOriginalBytes) {
    RetransmissionBuffer buf(512, 3000);
    auto pkt = makePacket(100, 100);
    buf.store(100, pkt, 0);

    auto got = buf.get(100, 10);
    EXPECT_EQ(got, pkt);            // 原字节补发（同 seq 同内容）
    EXPECT_EQ(buf.retransmittedCount(), 1u);
    EXPECT_EQ(buf.missCount(), 0u);
}

TEST(RetransmissionBufferTest, MissingSeqCountsMiss) {
    RetransmissionBuffer buf(512, 3000);
    auto got = buf.get(999, 0);
    EXPECT_TRUE(got.empty());       // 未命中返回空
    EXPECT_EQ(buf.missCount(), 1u); // 计入 NACK 未命中
}

TEST(RetransmissionBufferTest, CapacityEvictsOldest) {
    RetransmissionBuffer buf(2, 3000);  // 容量 2
    auto p1 = makePacket(1, 50), p2 = makePacket(2, 50), p3 = makePacket(3, 50);
    buf.store(1, p1, 0);
    buf.store(2, p2, 0);
    buf.store(3, p3, 1);  // 触发淘汰最老的 seq=1

    EXPECT_TRUE(buf.get(1, 2).empty());  // 已被淘汰 → miss
    EXPECT_FALSE(buf.get(2, 2).empty()); // 仍在
    EXPECT_FALSE(buf.get(3, 2).empty());
    EXPECT_EQ(buf.missCount(), 1u);
}

TEST(RetransmissionBufferTest, TtlEvictsExpired) {
    RetransmissionBuffer buf(512, 300);  // TTL 300ms
    buf.store(100, makePacket(100, 50), 0);

    EXPECT_FALSE(buf.get(100, 200).empty());  // 200ms 内有效
    EXPECT_TRUE(buf.get(100, 400).empty());   // 超过 300ms → 过期淘汰
}

TEST(RetransmissionBufferTest, LateStoreOfOldSeqAllowed) {
    // 发送侧乱序（如重传与正常发送并发）也允许存储，按插入序淘汰
    RetransmissionBuffer buf(512, 3000);
    buf.store(100, makePacket(100, 50), 0);
    buf.store(99, makePacket(99, 50), 1);
    EXPECT_FALSE(buf.get(99, 2).empty());
    EXPECT_FALSE(buf.get(100, 2).empty());
}
