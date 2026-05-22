#pragma once

#include "media/rtp/rtp_packet.h"
#include <cstdint>
#include <map>
#include <vector>

namespace crystal {

class JitterBuffer {
public:
    explicit JitterBuffer(uint32_t targetDelayMs = 40);

    void insert(const RtpPacket& pkt);
    std::vector<RtpPacket> consume();

    uint64_t lostPacketCount() const;
    uint64_t receivedPacketCount() const;
    double lossRate() const;

private:
    uint32_t targetDelayMs_;
    std::map<uint16_t, RtpPacket> buffer_;
    uint16_t expectedSeq_ = 0;
    bool firstPacket_ = true;
    uint64_t lostCount_ = 0;
    uint64_t receivedCount_ = 0;
};

} // namespace crystal
