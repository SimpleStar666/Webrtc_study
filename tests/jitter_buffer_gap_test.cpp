// ============================================================================
// jitter_buffer_gap_test.cpp — insert() 丢失序列号返回值测试
// ============================================================================
// 覆盖：间隙返回缺失 seq、大间隙不上报（防 NACK 风暴）、迟到包不触发。
// 注：RtpPacket 的 setter（setPayloadType 等）已存在，见 rtp_packet.h。
// ============================================================================
#include <gtest/gtest.h>
#include "media/rtp/jitter_buffer.h"

using namespace crystal;

static RtpPacket makePkt(uint16_t seq, uint8_t pt = 96) {
    RtpPacket p;
    p.setPayloadType(pt);
    p.setSequenceNumber(seq);
    p.setTimestamp(seq * 3000);
    p.setSsrc(0x12345678);
    std::vector<uint8_t> payload(10, 0xAB);
    p.setPayload(payload);
    return p;
}

TEST(JitterBufferGapTest, ReturnsMissingSeqsOnGap) {
    JitterBuffer jb;
    EXPECT_TRUE(jb.insert(makePkt(100)).empty());        // 首包无间隙
    EXPECT_EQ(jb.insert(makePkt(103)),                   // 缺 101,102
              (std::vector<uint16_t>{101, 102}));
    EXPECT_EQ(jb.insert(makePkt(105)),                   // 缺 104
              (std::vector<uint16_t>{104}));
}

TEST(JitterBufferGapTest, LatePacketReportsNothing) {
    JitterBuffer jb;
    jb.insert(makePkt(100));
    jb.insert(makePkt(103));  // 101,102 报丢失
    EXPECT_TRUE(jb.insert(makePkt(101)).empty());  // 迟到包只是恢复
}

TEST(JitterBufferGapTest, HugeGapNotReported) {
    JitterBuffer jb;
    jb.insert(makePkt(100));
    // diff=99 > kMaxNackGap(64)：疑似码流重启/暂停，不做 NACK
    auto missing = jb.insert(makePkt(200));
    EXPECT_TRUE(missing.empty());
    // 但丢包统计仍然计数
    EXPECT_EQ(jb.lostPacketCount(), 99u);
}

TEST(JitterBufferGapTest, InOrderNoGap) {
    JitterBuffer jb;
    jb.insert(makePkt(1000));
    jb.insert(makePkt(1001));
    jb.insert(makePkt(1002));
    EXPECT_TRUE(jb.insert(makePkt(1003)).empty());
}
