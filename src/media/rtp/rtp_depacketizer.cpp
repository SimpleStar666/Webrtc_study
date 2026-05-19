#include "media/rtp/rtp_depacketizer.h"
#include "utils/logger.h"

namespace crystal {

RtpDepacketizer::RtpDepacketizer() = default;

std::vector<std::vector<uint8_t>> RtpDepacketizer::depacketizeH264(
    const RtpPacket& pkt) {

    std::vector<std::vector<uint8_t>> result;
    const auto& payload = pkt.payload();

    if (payload.empty()) return result;

    uint8_t nalType = payload[0] & 0x1F;

    if (nalType >= 1 && nalType <= 23) {
        if (fuStarted_) {
            Logger::warn("Dropping incomplete FU-A buffer on single NAL arrival");
            fuStarted_ = false;
            fuBuffer_.clear();
        }
        result.push_back(payload);
    } else if (nalType == 28) {
        if (payload.size() < 2) {
            Logger::warn("FU-A packet too short");
            return result;
        }

        uint8_t fuHeader = payload[1];
        bool startBit = (fuHeader & 0x80) != 0;
        bool endBit = (fuHeader & 0x40) != 0;
        uint8_t originalNalType = fuHeader & 0x1F;

        if (startBit) {
            if (fuStarted_) {
                Logger::warn("New FU-A start while previous incomplete, dropping old");
            }
            fuStarted_ = true;
            fuBuffer_.clear();
            uint8_t nalRefIdc = (payload[0] & 0x60);
            uint8_t reconstructedNal = nalRefIdc | originalNalType;
            fuBuffer_.push_back(reconstructedNal);
            if (payload.size() > 2) {
                fuBuffer_.insert(fuBuffer_.end(), payload.begin() + 2,
                                 payload.end());
            }
        } else if (fuStarted_) {
            if (payload.size() > 2) {
                fuBuffer_.insert(fuBuffer_.end(), payload.begin() + 2,
                                 payload.end());
            }

            if (endBit) {
                fuStarted_ = false;
                result.push_back(std::move(fuBuffer_));
                fuBuffer_.clear();
            }
        } else {
            Logger::warn("FU-A middle/end without start, dropping");
        }
    } else if (nalType == 24) {
        Logger::debug("STAP-A packet received (not yet fully supported, extracting single NAL)");
    } else {
        Logger::warn("Unsupported NAL type in RTP: {}", static_cast<int>(nalType));
    }

    return result;
}

std::vector<std::vector<uint8_t>> RtpDepacketizer::depacketizeOpus(
    const RtpPacket& pkt) {
    std::vector<std::vector<uint8_t>> result;
    result.push_back(pkt.payload());
    return result;
}

} // namespace crystal
