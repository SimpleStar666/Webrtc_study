#include <gtest/gtest.h>
#include "media/rtp/rtp_packet.h"

TEST(RtpPacketTest, DefaultConstructorHasVersion2) {
    crystal::RtpPacket pkt;
    EXPECT_EQ(pkt.version(), 2);
    EXPECT_EQ(pkt.padding(), false);
    EXPECT_EQ(pkt.extension(), false);
    EXPECT_EQ(pkt.csrcCount(), 0);
    EXPECT_EQ(pkt.marker(), false);
    EXPECT_EQ(pkt.payloadType(), 0);
    EXPECT_EQ(pkt.sequenceNumber(), 0);
    EXPECT_EQ(pkt.timestamp(), 0u);
    EXPECT_EQ(pkt.ssrc(), 0u);
}

TEST(RtpPacketTest, SetAndGetFields) {
    crystal::RtpPacket pkt;
    pkt.setMarker(true);
    pkt.setPayloadType(96);
    pkt.setSequenceNumber(12345);
    pkt.setTimestamp(90000);
    pkt.setSsrc(0xDEADBEEF);

    EXPECT_TRUE(pkt.marker());
    EXPECT_EQ(pkt.payloadType(), 96);
    EXPECT_EQ(pkt.sequenceNumber(), 12345);
    EXPECT_EQ(pkt.timestamp(), 90000u);
    EXPECT_EQ(pkt.ssrc(), 0xDEADBEEFu);
}

TEST(RtpPacketTest, SerializeAndParse) {
    crystal::RtpPacket original;
    original.setMarker(true);
    original.setPayloadType(96);
    original.setSequenceNumber(100);
    original.setTimestamp(5400000);
    original.setSsrc(0x12345678);
    original.setPayload({0x00, 0x01, 0x02, 0x03});

    auto data = original.serialize();

    crystal::RtpPacket parsed;
    ASSERT_TRUE(parsed.parse(data.data(), data.size()));

    EXPECT_EQ(parsed.version(), 2);
    EXPECT_TRUE(parsed.marker());
    EXPECT_EQ(parsed.payloadType(), 96);
    EXPECT_EQ(parsed.sequenceNumber(), 100);
    EXPECT_EQ(parsed.timestamp(), 5400000u);
    EXPECT_EQ(parsed.ssrc(), 0x12345678u);
    EXPECT_EQ(parsed.payload().size(), 4u);
    EXPECT_EQ(parsed.payload()[0], 0x00);
    EXPECT_EQ(parsed.payload()[3], 0x03);
}

TEST(RtpPacketTest, HeaderSizeIs12Bytes) {
    crystal::RtpPacket pkt;
    EXPECT_EQ(pkt.headerSize(), 12u);
}

TEST(RtpPacketTest, TotalSizeIsHeaderPlusPayload) {
    crystal::RtpPacket pkt;
    pkt.setPayload({1, 2, 3, 4, 5});
    EXPECT_EQ(pkt.totalSize(), 17u);
}
