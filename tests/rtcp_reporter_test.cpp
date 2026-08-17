// ============================================================================
// rtcp_reporter_test.cpp — SR/RR 统计器单元测试
// ============================================================================
// 覆盖：接收侧丢包/回绕/抖动（RFC 3550 A.1/A.8）、发送侧计数、RTT 计算
// （LSR/DLSR 法，注入时钟避免真实时间）。
// ============================================================================
#include <gtest/gtest.h>
#include "media/rtcp/rtcp_reporter.h"

using namespace crystal;

// 毫秒 → NTP（测试辅助，+500 四舍五入避免截断误差累积进抖动）
static uint64_t msToNtp(uint64_t ms) {
    return ((static_cast<uint64_t>(ms) << 32) + 500) / 1000;
}

// ----------------------------------------------------------------------------
// RecvSideReporter：丢包统计（RFC 3550 A.1）
// ----------------------------------------------------------------------------
TEST(RecvSideTest, LossAndFractionLost) {
    RecvSideReporter recv(90000);  // 视频时钟
    // 收到 seq 1,2,3,4,6,7,8,9,10（5 丢失），乱序喂入
    uint16_t seqs[] = {2, 1, 4, 3, 6, 8, 7, 10, 9};
    uint64_t t = 0;
    for (uint16_t s : seqs) {
        recv.onPacketReceived(0xAA000001, s, s * 3000, msToNtp(t));
        t += 33;
    }

    ReportBlock blk;
    ASSERT_TRUE(recv.buildBlock(msToNtp(t), blk));
    // expected = 10-1+1 = 10, received = 9 → 累计丢 1
    EXPECT_EQ(blk.cumulativeLost, 1u);
    // 间隔统计：期望增量 10，接收增量 9，丢 1 → fraction = 1*256/10 = 25
    EXPECT_EQ(blk.fractionLost, 25u);
    EXPECT_DOUBLE_EQ(recv.lossRate(), 0.1);  // 1/10
}

TEST(RecvSideTest, SequenceWraparound) {
    RecvSideReporter recv(90000);
    recv.onPacketReceived(1, 65534, 0, msToNtp(0));
    recv.onPacketReceived(1, 65535, 3000, msToNtp(33));
    recv.onPacketReceived(1, 0, 6000, msToNtp(66));     // 回绕
    recv.onPacketReceived(1, 1, 9000, msToNtp(99));

    ReportBlock blk;
    ASSERT_TRUE(recv.buildBlock(msToNtp(132), blk));
    // 扩展最高序列号 = 65536+1 = 65537；期望 4 收 4 → 无丢失
    EXPECT_EQ(blk.extHighestSeq, 65537u);
    EXPECT_EQ(blk.cumulativeLost, 0u);
}

TEST(RecvSideTest, JitterFollowsTransitVariation) {
    RecvSideReporter recv(1000);  // 时钟 1000Hz，1 tick = 1ms
    // 包间隔 20ms、RTP ts 增量 20 → transit 恒定，抖动为 0
    recv.onPacketReceived(1, 1, 0,  msToNtp(0));
    recv.onPacketReceived(1, 2, 20, msToNtp(20));
    EXPECT_DOUBLE_EQ(recv.jitterMs(), 0.0);
    // 第 3 包正常应在 40ms 到达（间隔=ts 增量 20），实际 50ms → 晚 10ms
    // transit: 20-20=0 → 50-40=10，D=10 → jitter = (10-0)/16 = 0.625ms
    recv.onPacketReceived(1, 3, 40, msToNtp(50));
    EXPECT_NEAR(recv.jitterMs(), 0.625, 0.01);
}

TEST(RecvSideTest, LsrDlsrFromSenderReport) {
    RecvSideReporter recv(90000);
    recv.onPacketReceived(0xAA000001, 1, 3000, msToNtp(0));  // 激活该流
    recv.onSenderReport(0xAA000001, msToNtp(1000));  // 记住 SR 到达时刻
    ReportBlock blk;
    ASSERT_TRUE(recv.buildBlock(msToNtp(2500), blk));
    EXPECT_EQ(blk.lsr, ntpMiddle32(msToNtp(1000)));
    // 1500ms = 1.5s → DLSR = 1.5 * 65536 = 98304
    EXPECT_EQ(blk.dlsr, 98304u);
}

// ----------------------------------------------------------------------------
// SendSideReporter：计数与 RTT
// ----------------------------------------------------------------------------
TEST(SendSideTest, CountsAndSenderReportRoundTrip) {
    SendSideReporter send(0x12345678, "crystal-test");
    send.onPacketSent(100, 1500, 3000);
    send.onPacketSent(101, 1200, 6000);

    auto report = send.buildReport(msToNtp(5000), {});  // 无接收报告块
    // 复合包含 SR + SDES 两个子包，只捕获 SR（后者会覆盖前者）
    RtcpPacket got;
    int subpackets = 0;
    ASSERT_TRUE(parseRtcpCompound(report.data(), report.size(),
                                  [&](const RtcpPacket& p) {
                                      subpackets++;
                                      if (p.kind == RtcpKind::SenderReport)
                                          got = p;
                                  }));
    EXPECT_EQ(subpackets, 2);  // SR + SDES
    ASSERT_EQ(got.kind, RtcpKind::SenderReport);
    EXPECT_EQ(got.sr.ssrc, 0x12345678u);
    EXPECT_EQ(got.sr.packetCount, 2u);
    EXPECT_EQ(got.sr.octetCount, 2700u);
    EXPECT_EQ(got.sr.rtpTimestamp, 6000u);  // 最近一个包的 ts
}

TEST(SendSideTest, RttFromReceiverReport) {
    SendSideReporter send(0x12345678, "c");
    uint64_t srNtp = msToNtp(10000);        // 我方 SR 发出（对端以此填 LSR）
    ReceiverReport rr;
    rr.ssrc = 0x87654321;
    ReportBlock blk;
    blk.ssrc = 0x12345678;                  // 针对我方视频流
    blk.lsr = ntpMiddle32(srNtp);           // 对端收到我方 SR 的时刻
    blk.dlsr = 65536;                       // 对端延迟 1.0s 后发出 RR
    rr.blocks.push_back(blk);

    // 我方在 SR 发出 2.0s 后收到 RR → RTT = 2.0 - 1.0 = 1.0s
    send.onReceiverReport(rr, srNtp + (2ULL << 32));
    EXPECT_TRUE(send.hasRtt());
    EXPECT_NEAR(send.rttMs(), 1000.0, 1.0);
}

TEST(SendSideTest, RttSkippedWhenLsrZero) {
    SendSideReporter send(0x12345678, "c");
    ReceiverReport rr;
    rr.ssrc = 0x87654321;
    ReportBlock blk;
    blk.ssrc = 0x12345678;
    blk.lsr = 0;  // 对端从未收到我方 SR
    rr.blocks.push_back(blk);

    send.onReceiverReport(rr, msToNtp(1000));
    EXPECT_FALSE(send.hasRtt());
}
