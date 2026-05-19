#include "media/rtp/jitter_buffer.h"
#include "utils/logger.h"

namespace crystal {

JitterBuffer::JitterBuffer(uint32_t targetDelayMs)
    : targetDelayMs_(targetDelayMs) {}

void JitterBuffer::insert(const RtpPacket& pkt) {
    receivedCount_++;

    if (firstPacket_) {
        expectedSeq_ = pkt.sequenceNumber();
        firstPacket_ = false;
        buffer_[pkt.sequenceNumber()] = pkt;
        Logger::debug("JitterBuffer: first packet seq={}", pkt.sequenceNumber());
        return;
    }

    uint16_t seq = pkt.sequenceNumber();
    int16_t diff = static_cast<int16_t>(seq - expectedSeq_);

    if (diff == 0) {
        buffer_[seq] = pkt;
    } else if (diff > 0) {
        if (diff > 1) {
            lostCount_ += static_cast<uint64_t>(diff - 1);
            Logger::debug("JitterBuffer: gap detected, expected {} got {}, {} lost",
                          expectedSeq_, seq, diff - 1);
        }
        buffer_[seq] = pkt;
    } else {
        Logger::debug("JitterBuffer: late/duplicate packet seq={}", seq);
    }
}

std::vector<RtpPacket> JitterBuffer::consume() {
    std::vector<RtpPacket> result;

    if (buffer_.empty()) return result;

    auto it = buffer_.begin();
    while (it != buffer_.end()) {
        uint16_t seq = it->first;
        int16_t diff = static_cast<int16_t>(seq - expectedSeq_);

        if (diff <= 0) {
            result.push_back(it->second);
            expectedSeq_ = seq + 1;
            it = buffer_.erase(it);
        } else if (diff <= 3) {
            expectedSeq_ = seq;
            result.push_back(it->second);
            expectedSeq_ = seq + 1;
            it = buffer_.erase(it);
        } else {
            ++it;
        }
    }

    return result;
}

uint64_t JitterBuffer::lostPacketCount() const { return lostCount_; }
uint64_t JitterBuffer::receivedPacketCount() const { return receivedCount_; }

double JitterBuffer::lossRate() const {
    uint64_t total = receivedCount_ + lostCount_;
    if (total == 0) return 0.0;
    return static_cast<double>(lostCount_) / static_cast<double>(total);
}

} // namespace crystal
