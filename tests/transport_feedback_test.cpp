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

// ============================================================================
// TwccRecorder（接收端窗口收集，工程化升级 v2 接线新增）
// ============================================================================
#include "media/rtcp/twcc_recorder.h"

// 窗口未满 100ms：不产 feedback；满了才产，且样本完整
TEST(TwccRecorder, HoldsBefore100ms) {
    crystal::TwccRecorder r(0x1111, 0x2222);
    r.onPacket(100, 10.0);
    crystal::TwccFeedback fb;
    EXPECT_FALSE(r.buildFeedback(95.0, fb));   // 距首样本 85ms < 100ms
    ASSERT_TRUE(r.buildFeedback(110.0, fb));   // 距首样本 100ms → 产出
    EXPECT_EQ(fb.senderSsrc, 0x1111u);
    EXPECT_EQ(fb.mediaSsrc, 0x2222u);
    EXPECT_EQ(fb.baseSeq, 100u);
    ASSERT_EQ(fb.received.size(), 1u);
    EXPECT_EQ(fb.received[0].seq, 100u);
    EXPECT_TRUE(fb.lost.empty());
}

// 窗口内序号缺口 → 自动补 lost（TWCC 的逐包丢包检测来源）
TEST(TwccRecorder, GapFilledAsLost) {
    crystal::TwccRecorder r(0x1111, 0x2222);
    r.onPacket(10, 0.0);
    // 11 丢失
    r.onPacket(12, 25.0);
    crystal::TwccFeedback fb;
    ASSERT_TRUE(r.buildFeedback(200.0, fb));
    EXPECT_EQ(fb.lost, (std::vector<uint16_t>{11}));
    ASSERT_EQ(fb.received.size(), 2u);
    EXPECT_EQ(fb.received[0].seq, 10u);
    EXPECT_EQ(fb.received[1].seq, 12u);
}

// 积压 64 包：时间没到也立即产出（防反馈延迟过大）
TEST(TwccRecorder, BacklogOf64Flushes) {
    crystal::TwccRecorder r(0x1111, 0x2222);
    for (int i = 0; i < 64; ++i) r.onPacket(500 + i, i * 1.0);
    crystal::TwccFeedback fb;
    ASSERT_TRUE(r.buildFeedback(50.0, fb));    // 仅过 50ms，但包数已达上限
    EXPECT_EQ(fb.received.size(), 64u);
    EXPECT_TRUE(fb.lost.empty());
}

// 序号回绕：65534 → 65535 → 0 连续记录，缺口（65535）正确补 lost
TEST(TwccRecorder, SeqWraparound) {
    crystal::TwccRecorder r(0x1111, 0x2222);
    r.onPacket(65534, 0.0);
    // 65535 丢失
    r.onPacket(0, 20.0);   // 回绕
    crystal::TwccFeedback fb;
    ASSERT_TRUE(r.buildFeedback(300.0, fb));
    EXPECT_EQ(fb.baseSeq, 65534u);
    EXPECT_EQ(fb.lost, (std::vector<uint16_t>{65535}));
    ASSERT_EQ(fb.received.size(), 2u);
    EXPECT_EQ(fb.received[0].seq, 65534u);
    EXPECT_EQ(fb.received[1].seq, 0u);
}

// mediaSsrc 运行时可更新（对端视频 SSRC 发现后）
TEST(TwccRecorder, MediaSsrcUpdatable) {
    crystal::TwccRecorder r(0x1111, 0x2222);
    r.setMediaSsrc(0x3333);
    r.onPacket(7, 0.0);
    crystal::TwccFeedback fb;
    ASSERT_TRUE(r.buildFeedback(500.0, fb));
    EXPECT_EQ(fb.mediaSsrc, 0x3333u);
}

// 整链路：recorder（大数值绝对时钟）→ 编码 → 解码 → 时刻/丢失精确还原
// 验证会话时钟相对化：refTime 不因绝对时刻巨大而饱和
TEST(TwccRecorder, EndToEndThroughEncodeDecode) {
    crystal::TwccRecorder r(0x1111, 0x2222);
    r.onPacket(100, 8000000.0);   // steady 时钟毫秒，数值巨大
    r.onPacket(101, 8000012.5);   // +12.5ms
    r.onPacket(103, 8000040.0);   // 102 丢失
    crystal::TwccFeedback fb;
    ASSERT_TRUE(r.buildFeedback(8005000.0, fb));

    auto bytes = crystal::appendTransportFeedback(fb);
    crystal::TwccFeedback dec;
    crystal::parseRtcpCompound(bytes.data(), bytes.size(),
        [&](const crystal::RtcpPacket& p) {
            if (p.kind == crystal::RtcpKind::TransportFeedback) dec = p.twcc;
        });
    ASSERT_EQ(dec.received.size(), 3u);
    EXPECT_EQ(dec.lost, (std::vector<uint16_t>{102}));
    EXPECT_EQ(dec.senderSsrc, 0x1111u);
    EXPECT_EQ(dec.mediaSsrc, 0x2222u);
    // 到达时刻按会话原点相对化后精确还原（±1ms 量化容差）
    EXPECT_NEAR(dec.received[0].arrivalMs, 0.0, 1.0);
    EXPECT_NEAR(dec.received[1].arrivalMs, 12.5, 1.0);
    EXPECT_NEAR(dec.received[2].arrivalMs, 40.0, 1.0);
}
