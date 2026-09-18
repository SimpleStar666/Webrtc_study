// ============================================================================
// twcc_header_test.cpp — TWCC 扩展头单元测试（工程化升级 v2 新增）
// ============================================================================
// 验证 RFC 8285 one-byte header extension 格式的往返一致性：
//   setTwccSeq → serialize → parse → 读回 twccSeq
// 以及向后兼容：未设置扩展的包仍是纯 12 字节头。
//
// 【one-byte extension 布局（RFC 8285 Section 4.2）】
//   固定头后紧跟 4 字节扩展头：0xBE 0xDE + length（32bit 字数，不含本身）
//   之后是扩展块：块首字节 ID(4bit) | L(4bit=数据长度-1)
//   TWCC 用 ID=1，数据 2 字节序号 → L=1
//   扩展区按 4 字节对齐，尾部补零
// ============================================================================

#include <gtest/gtest.h>
#include "media/rtp/rtp_packet.h"

// 设置了 TWCC 序号 → 序列化 → 解析 → 应读回同一序号 + 字节布局逐项核对
TEST(TwccHeader, RoundTrip) {
    crystal::RtpPacket p;
    p.setPayloadType(96);
    p.setSequenceNumber(100);
    p.setTimestamp(90000);
    p.setSsrc(0x12345678);
    p.setTwccSeq(32167);
    p.setPayload({0x01, 0x02, 0x03});

    auto bytes = p.serialize();
    // 头部 = 12 固定 + 4 扩展头 + 4 TWCC 块（3 字节数据 + 1 字节补零）= 20 字节
    ASSERT_EQ(bytes.size(), 20u + 3u);

    // byte0 的 X 位应置位：V=2|P=0|X=1|CC=0 → 1001 0000 = 0x90
    EXPECT_EQ(bytes[0], 0x90);
    // byte1: M=0 | PT=96 → 0x60
    EXPECT_EQ(bytes[1], 0x60);

    // 扩展头：0xBE 0xDE + length=1（扩展数据 4 字节 / 4 - 1 = 1 个 32bit 字）
    EXPECT_EQ(bytes[12], 0xBE);
    EXPECT_EQ(bytes[13], 0xDE);
    EXPECT_EQ(bytes[14], 0x00);
    EXPECT_EQ(bytes[15], 0x01);
    // TWCC 块：ID=1 | L=1（数据 2 字节，L=2-1）| 序号大端
    EXPECT_EQ(bytes[16], 0x11);
    EXPECT_EQ(bytes[17], 32167 >> 8);
    EXPECT_EQ(bytes[18], 32167 & 0xFF);

    crystal::RtpPacket q;
    ASSERT_TRUE(q.parse(bytes.data(), bytes.size()));
    EXPECT_TRUE(q.extension());
    EXPECT_TRUE(q.hasTwcc());
    EXPECT_EQ(q.twccSeq(), 32167);
    EXPECT_EQ(q.sequenceNumber(), 100);
    EXPECT_EQ(q.payload().size(), 3u);
}

// 未设置 TWCC → 保持纯 12 字节头（向后兼容，音频流不打 TWCC）
TEST(TwccHeader, NoExtensionStillWorks) {
    crystal::RtpPacket p;
    p.setPayloadType(97);
    p.setSequenceNumber(7);
    p.setMarker(true);
    p.setPayload({0xAA});
    auto bytes = p.serialize();
    ASSERT_EQ(bytes.size(), 13u);
    EXPECT_EQ(bytes[1], 0x80 | 97);  // X=0 | M=1 | PT=97

    crystal::RtpPacket q;
    ASSERT_TRUE(q.parse(bytes.data(), bytes.size()));
    EXPECT_FALSE(q.extension());
    EXPECT_FALSE(q.hasTwcc());
}

// 序号回绕：65535 → 0
TEST(TwccHeader, SeqWraparound) {
    crystal::RtpPacket p;
    p.setTwccSeq(65535);
    auto bytes = p.serialize();
    crystal::RtpPacket q;
    ASSERT_TRUE(q.parse(bytes.data(), bytes.size()));
    EXPECT_EQ(q.twccSeq(), 65535);

    p.setTwccSeq(0);
    bytes = p.serialize();
    ASSERT_TRUE(q.parse(bytes.data(), bytes.size()));
    EXPECT_EQ(q.twccSeq(), 0);
}

// getTwccSeq 带返回值的版本（接线用：未设置时返回 false）
TEST(TwccHeader, GetTwccSeqReturnsFalseWhenAbsent) {
    crystal::RtpPacket p;
    p.setPayloadType(97);
    p.setSequenceNumber(1);
    p.setPayload({0x00});
    auto bytes = p.serialize();

    crystal::RtpPacket q;
    ASSERT_TRUE(q.parse(bytes.data(), bytes.size()));
    uint16_t seq = 999;
    EXPECT_FALSE(q.getTwccSeq(seq));
    EXPECT_EQ(seq, 999u);  // 未写入
}
