// ============================================================================
// transport_feedback_test.cpp — TWCC TransportFeedback 编解码测试（v2 新增）
// ============================================================================
// Google TWCC feedback 格式（draft-holmer-rmcat-feedback）：
//   头部 20 字节（含 RTCP 公共头）+ N*2B StatusVectorChunk + M*2B delta
// 简化策略：全用 2-bit symbol 的 StatusVectorChunk + 2 字节有符号 delta(250us)
// ============================================================================

#include <gtest/gtest.h>
#include "media/rtcp/transport_feedback.h"
#include "media/rtcp/rtcp_packet.h"

// 编码 → 解码 round-trip：3 个包（1 收到、1 丢失、1 收到）
TEST(TransportFeedback, RoundTrip) {
    crystal::TwccFeedback fb;
    fb.senderSsrc = 0x1111;
    fb.mediaSsrc = 0x2222;
    // 样本：seq 100 于 10ms 到达，101 丢失，102 于 35ms 到达
    fb.baseSeq = 100;
    fb.refTimeMs = 10;  // 基准时刻（1/64ms 网格）
    fb.received = {{100, 10.0}, {102, 35.0}};  // (seq, arrivalMs) 无序容忍
    fb.lost = {101};

    auto bytes = crystal::appendTransportFeedback(fb);

    // RTCP 公共头检查：V=2, FMT=15, PT=205
    EXPECT_EQ(bytes[0] >> 6, 2);
    EXPECT_EQ(bytes[0] & 0x1F, 15);
    EXPECT_EQ(bytes[1], 205);

    // 解码（走复合包解析路径）
    crystal::TwccFeedback decoded;
    int hit = 0;
    crystal::parseRtcpCompound(bytes.data(), bytes.size(),
        [&](const crystal::RtcpPacket& p) {
            if (p.kind == crystal::RtcpKind::TransportFeedback) {
                decoded = p.twcc;
                hit++;
            }
        });
    ASSERT_EQ(hit, 1);
    EXPECT_EQ(decoded.senderSsrc, 0x1111u);
    EXPECT_EQ(decoded.mediaSsrc, 0x2222u);
    EXPECT_EQ(decoded.baseSeq, 100u);

    // 到达时刻还原：refTime 基准 + delta 累加（250us 网格有量化误差，±1ms 内）
    ASSERT_EQ(decoded.received.size(), 2u);
    EXPECT_NEAR(decoded.received[0].arrivalMs, 10.0, 1.0);
    EXPECT_NEAR(decoded.received[1].arrivalMs, 35.0, 1.0);
    EXPECT_EQ(decoded.lost, (std::vector<uint16_t>{101}));
}

// 序号回绕 round-trip：base=65534, 丢 0
TEST(TransportFeedback, Wraparound) {
    crystal::TwccFeedback fb;
    fb.baseSeq = 65534;
    fb.refTimeMs = 5;
    fb.received = {{65534, 5.0}, {65535, 8.0}};
    fb.lost = {0};  // 65534, 65535, 0 三个包中 0 丢失
    auto bytes = crystal::appendTransportFeedback(fb);

    crystal::TwccFeedback decoded;
    crystal::parseRtcpCompound(bytes.data(), bytes.size(),
        [&](const crystal::RtcpPacket& p) {
            if (p.kind == crystal::RtcpKind::TransportFeedback) decoded = p.twcc;
        });
    EXPECT_EQ(decoded.lost, (std::vector<uint16_t>{0}));
    ASSERT_EQ(decoded.received.size(), 2u);
}

// 长窗口：30 个包（超过 4 个 chunk × 7 symbol，验证多 chunk 拼接）
TEST(TransportFeedback, MultiChunk) {
    crystal::TwccFeedback fb;
    fb.baseSeq = 1000;
    fb.refTimeMs = 0;
    for (uint32_t i = 0; i < 30; i += 2) {
        fb.received.push_back({static_cast<uint16_t>(1000 + i), i * 1.0});   // 偶数包到达
        fb.lost.push_back(static_cast<uint16_t>(1000 + i + 1));  // 奇数包丢失
    }
    auto bytes = crystal::appendTransportFeedback(fb);
    crystal::TwccFeedback decoded;
    crystal::parseRtcpCompound(bytes.data(), bytes.size(),
        [&](const crystal::RtcpPacket& p) {
            if (p.kind == crystal::RtcpKind::TransportFeedback) decoded = p.twcc;
        });
    EXPECT_EQ(decoded.received.size(), 15u);
    EXPECT_EQ(decoded.lost.size(), 15u);
}

// 多子包复合：TransportFeedback 与 RR 拼在一个复合包里，各自解析互不干扰
TEST(TransportFeedback, CompoundWithOtherRtcp) {
    crystal::TwccFeedback fb;
    fb.senderSsrc = 0x3333;
    fb.mediaSsrc = 0x4444;
    fb.baseSeq = 10;
    fb.refTimeMs = 20;
    fb.received = {{10, 20.5}, {11, 26.0}};

    crystal::ReceiverReport rr;
    rr.ssrc = 0x5555;

    auto fbBytes = crystal::appendTransportFeedback(fb);
    std::vector<uint8_t> compound;
    crystal::appendReceiverReport(compound, rr);
    compound.insert(compound.end(), fbBytes.begin(), fbBytes.end());

    int rrHit = 0, twccHit = 0;
    crystal::TwccFeedback decoded;
    crystal::parseRtcpCompound(compound.data(), compound.size(),
        [&](const crystal::RtcpPacket& p) {
            if (p.kind == crystal::RtcpKind::ReceiverReport) rrHit++;
            if (p.kind == crystal::RtcpKind::TransportFeedback) {
                decoded = p.twcc;
                twccHit++;
            }
        });
    EXPECT_EQ(rrHit, 1);
    EXPECT_EQ(twccHit, 1);
    ASSERT_EQ(decoded.received.size(), 2u);
    EXPECT_NEAR(decoded.received[1].arrivalMs, 26.0, 1.0);
    EXPECT_TRUE(decoded.lost.empty());
}

// 空窗口：没有任何样本也应编码/解析成功（statusCount=0）
TEST(TransportFeedback, EmptyWindow) {
    crystal::TwccFeedback fb;
    fb.baseSeq = 500;
    fb.refTimeMs = 100;
    auto bytes = crystal::appendTransportFeedback(fb);

    crystal::TwccFeedback decoded;
    int hit = 0;
    crystal::parseRtcpCompound(bytes.data(), bytes.size(),
        [&](const crystal::RtcpPacket& p) {
            if (p.kind == crystal::RtcpKind::TransportFeedback) {
                decoded = p.twcc;
                hit++;
            }
        });
    ASSERT_EQ(hit, 1);
    EXPECT_TRUE(decoded.received.empty());
    EXPECT_TRUE(decoded.lost.empty());
}
