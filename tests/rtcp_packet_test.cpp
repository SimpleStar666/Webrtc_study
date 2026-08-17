// ============================================================================
// rtcp_packet_test.cpp — RTCP 协议层单元测试
// ============================================================================
// 覆盖：去复用规则、SR/RR/NACK/PLI/SDES round-trip、PID+BLP 位图、
//       复合包迭代、畸形包安全拒绝。
// ============================================================================
#include <gtest/gtest.h>
#include "media/rtcp/rtcp_packet.h"

using namespace crystal;

// 毫秒 → NTP（测试辅助，避免浮点误差）
static uint64_t msToNtp(uint64_t ms) {
    return (static_cast<uint64_t>(ms) << 32) / 1000;
}

// ----------------------------------------------------------------------------
// 去复用规则（RFC 5761：第二字节 ∈ [192,223] 为 RTCP）
// ----------------------------------------------------------------------------
TEST(RtcpDemuxRule, DistinguishesRtpAndRtcp) {
    // RTP：V=2 PT=96（byte1=0x60=96）→ 非 RTCP
    uint8_t rtp[] = {0x80, 0x60, 0x00, 0x01};
    EXPECT_FALSE(isRtcpPacket(rtp, sizeof(rtp)));
    // RTP：M=1 PT=96（byte1=224）→ 非 RTCP
    uint8_t rtpMarked[] = {0x80, 0xE0, 0x00, 0x01};
    EXPECT_FALSE(isRtcpPacket(rtpMarked, sizeof(rtpMarked)));
    // RTCP：RR（byte1=201）
    uint8_t rr[] = {0x81, 0xC9, 0x00, 0x01};
    EXPECT_TRUE(isRtcpPacket(rr, sizeof(rr)));
    // RTCP：RTPFB NACK（byte1=205）
    uint8_t nack[] = {0x81, 0xCD, 0x00, 0x02};
    EXPECT_TRUE(isRtcpPacket(nack, sizeof(nack)));
}

TEST(RtcpDemuxRule, BoundaryValues) {
    EXPECT_TRUE(isRtcpPacket((const uint8_t*)"\x80\xc0", 2));  // 192 → RTCP
    EXPECT_TRUE(isRtcpPacket((const uint8_t*)"\x80\xdf", 2));  // 223 → RTCP
    EXPECT_FALSE(isRtcpPacket((const uint8_t*)"\x80\xbf", 2)); // 191 → 非
    EXPECT_FALSE(isRtcpPacket((const uint8_t*)"\x80\xe0", 2)); // 224 → 非
    EXPECT_FALSE(isRtcpPacket((const uint8_t*)"\x80", 1));     // 太短 → 非
}

// ----------------------------------------------------------------------------
// SR round-trip
// ----------------------------------------------------------------------------
TEST(SenderReportTest, SerializeParseRoundTrip) {
    SenderReport sr;
    sr.ssrc = 0x12345678;
    sr.ntpTimestamp = msToNtp(123456);
    sr.rtpTimestamp = 90000;
    sr.packetCount = 42;
    sr.octetCount = 123456;
    ReportBlock blk;
    blk.ssrc = 0x87654321;
    blk.fractionLost = 25;
    blk.cumulativeLost = 3;
    blk.extHighestSeq = 0x0001FFFF;
    blk.jitter = 7;
    blk.lsr = 0x11223344;
    blk.dlsr = 65536;
    sr.blocks.push_back(blk);

    std::vector<uint8_t> buf;
    appendSenderReport(buf, sr);
    EXPECT_EQ(buf.size(), 28u + 24u);  // SR 头 28 字节 + 1 个报告块

    RtcpPacket got;
    ASSERT_TRUE(parseRtcpCompound(buf.data(), buf.size(),
                                  [&](const RtcpPacket& p) { got = p; }));
    EXPECT_EQ(got.kind, RtcpKind::SenderReport);
    EXPECT_EQ(got.sr.ssrc, sr.ssrc);
    EXPECT_EQ(got.sr.ntpTimestamp, sr.ntpTimestamp);
    EXPECT_EQ(got.sr.rtpTimestamp, sr.rtpTimestamp);
    EXPECT_EQ(got.sr.packetCount, sr.packetCount);
    EXPECT_EQ(got.sr.octetCount, sr.octetCount);
    ASSERT_EQ(got.sr.blocks.size(), 1u);
    EXPECT_EQ(got.sr.blocks[0].ssrc, blk.ssrc);
    EXPECT_EQ(got.sr.blocks[0].fractionLost, blk.fractionLost);
    EXPECT_EQ(got.sr.blocks[0].cumulativeLost, blk.cumulativeLost);
    EXPECT_EQ(got.sr.blocks[0].extHighestSeq, blk.extHighestSeq);
    EXPECT_EQ(got.sr.blocks[0].jitter, blk.jitter);
    EXPECT_EQ(got.sr.blocks[0].lsr, blk.lsr);
    EXPECT_EQ(got.sr.blocks[0].dlsr, blk.dlsr);
}

// ----------------------------------------------------------------------------
// RR round-trip
// ----------------------------------------------------------------------------
TEST(ReceiverReportTest, SerializeParseRoundTrip) {
    ReceiverReport rr;
    rr.ssrc = 0xAAAA0001;
    ReportBlock blk;
    blk.ssrc = 0xBBBB0002;
    blk.fractionLost = 128;
    blk.cumulativeLost = 100;
    blk.extHighestSeq = 65600;  // 含一次回绕
    rr.blocks.push_back(blk);

    std::vector<uint8_t> buf;
    appendReceiverReport(buf, rr);
    EXPECT_EQ(buf.size(), 8u + 24u);

    RtcpPacket got;
    ASSERT_TRUE(parseRtcpCompound(buf.data(), buf.size(),
                                  [&](const RtcpPacket& p) { got = p; }));
    EXPECT_EQ(got.kind, RtcpKind::ReceiverReport);
    EXPECT_EQ(got.rr.ssrc, rr.ssrc);
    ASSERT_EQ(got.rr.blocks.size(), 1u);
    EXPECT_EQ(got.rr.blocks[0].fractionLost, 128u);
    EXPECT_EQ(got.rr.blocks[0].extHighestSeq, 65600u);
}

// ----------------------------------------------------------------------------
// NACK：PID+BLP 位图
// ----------------------------------------------------------------------------
TEST(NackBitmapTest, BuildAndExpandRoundTrip) {
    // 连续丢失 100..110（11 个包）+ 孤立的 200
    std::vector<uint16_t> lost = {100, 101, 102, 103, 104, 105,
                                  106, 107, 108, 109, 110, 200};
    auto entries = NackPacket::buildEntries(lost);
    // 100..110 可被一个条目（PID=100, BLP 覆盖 101..110=10 个位）覆盖，
    // 200 超出 100+16 范围 → 新条目
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].pid, 100);
    EXPECT_EQ(entries[0].blp, 0x03FF);  // 位 0..9 全 1
    EXPECT_EQ(entries[1].pid, 200);
    EXPECT_EQ(entries[1].blp, 0);

    auto expanded = NackPacket::expandEntries(entries);
    EXPECT_EQ(expanded, lost);  // 展开后与输入一致（已排序去重）
}

TEST(NackBitmapTest, HandlesWraparound) {
    // 回绕场景：65534, 65535, 0, 1 连续丢失
    std::vector<uint16_t> lost = {65534, 65535, 0, 1};
    auto entries = NackPacket::buildEntries(lost);
    ASSERT_EQ(entries.size(), 1u);  // uint16 空间内仍然连续
    EXPECT_EQ(entries[0].pid, 65534);
    auto expanded = NackPacket::expandEntries(entries);
    EXPECT_EQ(expanded, (std::vector<uint16_t>{65534, 65535, 0, 1}));
}

TEST(NackPacketTest, SerializeParseRoundTrip) {
    NackPacket nack;
    nack.senderSsrc = 0x11111111;
    nack.mediaSsrc = 0x22222222;
    nack.entries = NackPacket::buildEntries({500, 501, 502});

    std::vector<uint8_t> buf;
    appendNack(buf, nack);
    EXPECT_EQ(buf.size(), 12u + 4u);  // 头 12 字节 + 1 个 FCI

    RtcpPacket got;
    ASSERT_TRUE(parseRtcpCompound(buf.data(), buf.size(),
                                  [&](const RtcpPacket& p) { got = p; }));
    EXPECT_EQ(got.kind, RtcpKind::Nack);
    EXPECT_EQ(got.nack.senderSsrc, nack.senderSsrc);
    EXPECT_EQ(got.nack.mediaSsrc, nack.mediaSsrc);
    EXPECT_EQ(got.nack.entries, nack.entries);
}

// ----------------------------------------------------------------------------
// PLI round-trip
// ----------------------------------------------------------------------------
TEST(PliPacketTest, SerializeParseRoundTrip) {
    PliPacket pli;
    pli.senderSsrc = 0x33333333;
    pli.mediaSsrc = 0x44444444;

    std::vector<uint8_t> buf;
    appendPli(buf, pli);
    EXPECT_EQ(buf.size(), 12u);  // PLI 无 FCI，固定 12 字节

    RtcpPacket got;
    ASSERT_TRUE(parseRtcpCompound(buf.data(), buf.size(),
                                  [&](const RtcpPacket& p) { got = p; }));
    EXPECT_EQ(got.kind, RtcpKind::Pli);
    EXPECT_EQ(got.pli.senderSsrc, pli.senderSsrc);
    EXPECT_EQ(got.pli.mediaSsrc, pli.mediaSsrc);
}

// ----------------------------------------------------------------------------
// SDES + 复合包
// ----------------------------------------------------------------------------
TEST(CompoundTest, TwoSubPacketsIteratedInOrder) {
    SenderReport sr;
    sr.ssrc = 1;
    sr.ntpTimestamp = msToNtp(1000);
    SdesPacket sdes;
    sdes.ssrc = 1;
    sdes.cname = "crystal-test";

    std::vector<uint8_t> buf;
    appendSenderReport(buf, sr);
    appendSdes(buf, sdes);

    std::vector<RtcpKind> kinds;
    std::string cname;
    ASSERT_TRUE(parseRtcpCompound(buf.data(), buf.size(), [&](const RtcpPacket& p) {
        kinds.push_back(p.kind);
        if (p.kind == RtcpKind::Sdes) cname = p.sdes.cname;
    }));
    EXPECT_EQ(kinds, (std::vector<RtcpKind>{RtcpKind::SenderReport, RtcpKind::Sdes}));
    EXPECT_EQ(cname, "crystal-test");
}

TEST(CompoundTest, MalformedPacketRejected) {
    // length 字段声明超出实际数据 → 解析失败，不崩溃
    std::vector<uint8_t> buf = {0x80, 201, 0x00, 0x0A};  // 声明 44 字节实际只有 4
    int calls = 0;
    EXPECT_FALSE(parseRtcpCompound(buf.data(), buf.size(),
                                   [&](const RtcpPacket&) { calls++; }));
    EXPECT_EQ(calls, 0);
}
