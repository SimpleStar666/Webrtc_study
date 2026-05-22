#include <gtest/gtest.h>
#include "media/rtp/rtp_packetizer.h"

TEST(RtpPacketizerTest, SmallNalPackedAsSingleUnit) {
    crystal::RtpPacketizer pktizer(96, 90000, 0x12345678);

    std::vector<uint8_t> smallNal = {0x65, 0x88, 0x84, 0x00, 0x40};
    auto packets = pktizer.packetizeH264(smallNal, 3000);

    ASSERT_EQ(packets.size(), 1u);
    EXPECT_TRUE(packets[0].marker());
    EXPECT_EQ(packets[0].payloadType(), 96);
    EXPECT_EQ(packets[0].timestamp(), 3000u);
    EXPECT_EQ(packets[0].payload()[0], 0x65);
}

TEST(RtpPacketizerTest, LargeNalFragmentedWithFUA) {
    crystal::RtpPacketizer pktizer(96, 90000, 0x12345678);

    std::vector<uint8_t> largeNal(3000, 0xAB);
    largeNal[0] = 0x65;

    auto packets = pktizer.packetizeH264(largeNal, 5000);

    EXPECT_GT(packets.size(), 1u);

    EXPECT_EQ(packets[0].payload()[0] & 0x1F, 28);
    EXPECT_TRUE(packets[0].payload()[1] & 0x80);
    EXPECT_FALSE(packets[0].payload()[1] & 0x40);
    EXPECT_FALSE(packets[0].marker());

    for (size_t i = 1; i < packets.size() - 1; i++) {
        EXPECT_EQ(packets[i].payload()[0] & 0x1F, 28);
        EXPECT_FALSE(packets[i].payload()[1] & 0x80);
        EXPECT_FALSE(packets[i].payload()[1] & 0x40);
        EXPECT_FALSE(packets[i].marker());
    }

    EXPECT_EQ(packets.back().payload()[0] & 0x1F, 28);
    EXPECT_FALSE(packets.back().payload()[1] & 0x80);
    EXPECT_TRUE(packets.back().payload()[1] & 0x40);
    EXPECT_TRUE(packets.back().marker());

    for (const auto& pkt : packets) {
        EXPECT_EQ(pkt.timestamp(), 5000u);
        EXPECT_EQ(pkt.ssrc(), 0x12345678u);
    }
}

TEST(RtpPacketizerTest, SequenceNumbersIncrement) {
    crystal::RtpPacketizer pktizer(96, 90000, 0xABCD);

    std::vector<uint8_t> nal1 = {0x65, 0x01};
    std::vector<uint8_t> nal2 = {0x65, 0x02};

    auto pkts1 = pktizer.packetizeH264(nal1, 1000);
    auto pkts2 = pktizer.packetizeH264(nal2, 2000);

    uint16_t lastSeq = pkts1.back().sequenceNumber();
    uint16_t firstSeq = pkts2.front().sequenceNumber();
    EXPECT_EQ(firstSeq, static_cast<uint16_t>(lastSeq + 1));
}

TEST(RtpPacketizerTest, AudioPacketizeSinglePacket) {
    crystal::RtpPacketizer pktizer(97, 48000, 0xBEEFCAFE);

    std::vector<uint8_t> opusFrame(80, 0xCC);
    auto packets = pktizer.packetizeOpus(opusFrame, 960);

    ASSERT_EQ(packets.size(), 1u);
    EXPECT_TRUE(packets[0].marker());
    EXPECT_EQ(packets[0].payloadType(), 97);
    EXPECT_EQ(packets[0].timestamp(), 960u);
    EXPECT_EQ(packets[0].payload().size(), 80u);
}
