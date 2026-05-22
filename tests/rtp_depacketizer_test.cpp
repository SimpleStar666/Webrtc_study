#include <gtest/gtest.h>
#include "media/rtp/rtp_depacketizer.h"
#include "media/rtp/rtp_packetizer.h"

TEST(RtpDepacketizerTest, SingleNalReconstructed) {
    crystal::RtpPacketizer pktizer(96, 90000, 0x1234);
    crystal::RtpDepacketizer depktizer;

    std::vector<uint8_t> nal = {0x65, 0x88, 0x84, 0x00, 0x40};
    auto packets = pktizer.packetizeH264(nal, 3000);

    for (const auto& pkt : packets) {
        auto nals = depktizer.depacketizeH264(pkt);
        ASSERT_EQ(nals.size(), 1u);
        EXPECT_EQ(nals[0], nal);
    }
}

TEST(RtpDepacketizerTest, FUAFragmentsReassembled) {
    crystal::RtpPacketizer pktizer(96, 90000, 0x1234);
    crystal::RtpDepacketizer depktizer;

    std::vector<uint8_t> largeNal(3000, 0xAB);
    largeNal[0] = 0x65;

    auto packets = pktizer.packetizeH264(largeNal, 5000);

    std::vector<std::vector<uint8_t>> allNals;
    for (const auto& pkt : packets) {
        auto nals = depktizer.depacketizeH264(pkt);
        allNals.insert(allNals.end(), nals.begin(), nals.end());
    }

    ASSERT_EQ(allNals.size(), 1u);
    EXPECT_EQ(allNals[0], largeNal);
}

TEST(RtpDepacketizerTest, MultipleNalsProduceMultipleOutputs) {
    crystal::RtpPacketizer pktizer(96, 90000, 0x1234);
    crystal::RtpDepacketizer depktizer;

    std::vector<uint8_t> nal1 = {0x65, 0x01, 0x02};
    std::vector<uint8_t> nal2 = {0x65, 0x03, 0x04};

    auto pkts1 = pktizer.packetizeH264(nal1, 1000);
    auto pkts2 = pktizer.packetizeH264(nal2, 2000);

    std::vector<std::vector<uint8_t>> allNals;
    for (const auto& pkt : pkts1) {
        auto nals = depktizer.depacketizeH264(pkt);
        allNals.insert(allNals.end(), nals.begin(), nals.end());
    }
    for (const auto& pkt : pkts2) {
        auto nals = depktizer.depacketizeH264(pkt);
        allNals.insert(allNals.end(), nals.begin(), nals.end());
    }

    ASSERT_EQ(allNals.size(), 2u);
    EXPECT_EQ(allNals[0], nal1);
    EXPECT_EQ(allNals[1], nal2);
}

TEST(RtpDepacketizerTest, OpusFrameExtracted) {
    crystal::RtpPacketizer pktizer(97, 48000, 0xBEEF);
    crystal::RtpDepacketizer depktizer;

    std::vector<uint8_t> opusFrame(80, 0xCC);
    auto packets = pktizer.packetizeOpus(opusFrame, 960);

    ASSERT_EQ(packets.size(), 1u);
    auto frames = depktizer.depacketizeOpus(packets[0]);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0], opusFrame);
}
