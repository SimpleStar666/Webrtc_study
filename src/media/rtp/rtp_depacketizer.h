#pragma once

#include "media/rtp/rtp_packet.h"
#include <cstdint>
#include <vector>

namespace crystal {

class RtpDepacketizer {
public:
    RtpDepacketizer();

    std::vector<std::vector<uint8_t>> depacketizeH264(const RtpPacket& pkt);
    std::vector<std::vector<uint8_t>> depacketizeOpus(const RtpPacket& pkt);

private:
    std::vector<uint8_t> fuBuffer_;
    bool fuStarted_ = false;
};

} // namespace crystal
