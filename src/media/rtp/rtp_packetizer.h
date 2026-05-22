#pragma once

#include "media/rtp/rtp_packet.h"
#include <cstdint>
#include <vector>

namespace crystal {

class RtpPacketizer {
public:
    RtpPacketizer(uint8_t payloadType, uint32_t clockRate, uint32_t ssrc,
                  uint16_t startSeqNum = 0, size_t maxPacketSize = 1200);

    std::vector<RtpPacket> packetizeH264(const std::vector<uint8_t>& nalUnit,
                                          uint32_t timestamp);
    std::vector<RtpPacket> packetizeOpus(const std::vector<uint8_t>& opusFrame,
                                          uint32_t timestamp);

private:
    uint8_t payloadType_;
    uint32_t clockRate_;
    uint32_t ssrc_;
    uint16_t sequenceNumber_;
    size_t maxPacketSize_;
};

} // namespace crystal
