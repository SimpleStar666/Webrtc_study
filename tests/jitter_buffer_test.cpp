#include <gtest/gtest.h>
#include "media/rtp/jitter_buffer.h"

TEST(JitterBufferTest, InsertAndConsumeInOrder) {
    crystal::JitterBuffer jb(40);

    crystal::RtpPacket pkt1, pkt2, pkt3;
    pkt1.setSequenceNumber(100);
    pkt1.setTimestamp(90000);
    pkt2.setSequenceNumber(101);
    pkt2.setTimestamp(93000);
    pkt3.setSequenceNumber(102);
    pkt3.setTimestamp(96000);

    jb.insert(pkt1);
    jb.insert(pkt2);
    jb.insert(pkt3);

    auto out = jb.consume();
    ASSERT_GE(out.size(), 3u);
    EXPECT_EQ(out[0].sequenceNumber(), 100);
    EXPECT_EQ(out[1].sequenceNumber(), 101);
    EXPECT_EQ(out[2].sequenceNumber(), 102);
}

TEST(JitterBufferTest, ReordersOutOfOrderPackets) {
    crystal::JitterBuffer jb(40);

    crystal::RtpPacket pkt1, pkt2, pkt3;
    pkt1.setSequenceNumber(100);
    pkt1.setTimestamp(90000);
    pkt2.setSequenceNumber(102);
    pkt2.setTimestamp(96000);
    pkt3.setSequenceNumber(101);
    pkt3.setTimestamp(93000);

    jb.insert(pkt1);
    jb.insert(pkt2);
    jb.insert(pkt3);

    auto out = jb.consume();
    ASSERT_GE(out.size(), 3u);
    EXPECT_EQ(out[0].sequenceNumber(), 100);
    EXPECT_EQ(out[1].sequenceNumber(), 101);
    EXPECT_EQ(out[2].sequenceNumber(), 102);
}

TEST(JitterBufferTest, DetectsPacketLoss) {
    crystal::JitterBuffer jb(40);

    crystal::RtpPacket pkt1, pkt2;
    pkt1.setSequenceNumber(100);
    pkt1.setTimestamp(90000);
    pkt2.setSequenceNumber(103);
    pkt2.setTimestamp(99000);

    jb.insert(pkt1);
    jb.insert(pkt2);

    auto out = jb.consume();
    EXPECT_GT(jb.lostPacketCount(), 0u);
}

TEST(JitterBufferTest, StatsInitializedToZero) {
    crystal::JitterBuffer jb(40);
    EXPECT_EQ(jb.lostPacketCount(), 0u);
    EXPECT_DOUBLE_EQ(jb.lossRate(), 0.0);
}
