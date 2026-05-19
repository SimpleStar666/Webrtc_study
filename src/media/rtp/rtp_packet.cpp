#include "media/rtp/rtp_packet.h"
#include "utils/logger.h"
#include <cstring>
#include <stdexcept>

namespace crystal {

RtpPacket::RtpPacket() = default;

bool RtpPacket::parse(const uint8_t* data, size_t size) {
    if (size < 12) {
        Logger::error("RTP packet too short: {} bytes", size);
        return false;
    }

    uint8_t byte0 = data[0];
    uint8_t byte1 = data[1];

    if ((byte0 >> 6) != 2) {
        Logger::error("Invalid RTP version: {}", byte0 >> 6);
        return false;
    }

    uint8_t cc = byte0 & 0x0F;
    size_t header_len = 12 + cc * 4;

    if (size < header_len) {
        Logger::error("RTP packet too short for CSRC: {} < {}", size, header_len);
        return false;
    }

    marker_ = (byte1 & 0x80) != 0;
    payloadType_ = byte1 & 0x7F;
    sequenceNumber_ = (static_cast<uint16_t>(data[2]) << 8) | data[3];
    timestamp_ = (static_cast<uint32_t>(data[4]) << 24) |
                 (static_cast<uint32_t>(data[5]) << 16) |
                 (static_cast<uint32_t>(data[6]) << 8) |
                 data[7];
    ssrc_ = (static_cast<uint32_t>(data[8]) << 24) |
            (static_cast<uint32_t>(data[9]) << 16) |
            (static_cast<uint32_t>(data[10]) << 8) |
            data[11];

    size_t payload_len = size - header_len;
    if (payload_len > 0) {
        payload_.assign(data + header_len, data + header_len + payload_len);
    } else {
        payload_.clear();
    }

    return true;
}

std::vector<uint8_t> RtpPacket::serialize() const {
    std::vector<uint8_t> buf;
    buf.reserve(12 + payload_.size());

    uint8_t byte0 = (2 << 6) | 0x00;
    buf.push_back(byte0);

    uint8_t byte1 = (marker_ ? 0x80 : 0x00) | (payloadType_ & 0x7F);
    buf.push_back(byte1);

    buf.push_back(static_cast<uint8_t>(sequenceNumber_ >> 8));
    buf.push_back(static_cast<uint8_t>(sequenceNumber_ & 0xFF));

    buf.push_back(static_cast<uint8_t>(timestamp_ >> 24));
    buf.push_back(static_cast<uint8_t>((timestamp_ >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((timestamp_ >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(timestamp_ & 0xFF));

    buf.push_back(static_cast<uint8_t>(ssrc_ >> 24));
    buf.push_back(static_cast<uint8_t>((ssrc_ >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((ssrc_ >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(ssrc_ & 0xFF));

    buf.insert(buf.end(), payload_.begin(), payload_.end());
    return buf;
}

uint8_t RtpPacket::version() const { return 2; }
bool RtpPacket::padding() const { return false; }
bool RtpPacket::extension() const { return false; }
uint8_t RtpPacket::csrcCount() const { return 0; }
bool RtpPacket::marker() const { return marker_; }
uint8_t RtpPacket::payloadType() const { return payloadType_; }
uint16_t RtpPacket::sequenceNumber() const { return sequenceNumber_; }
uint32_t RtpPacket::timestamp() const { return timestamp_; }
uint32_t RtpPacket::ssrc() const { return ssrc_; }
const std::vector<uint8_t>& RtpPacket::payload() const { return payload_; }

void RtpPacket::setMarker(bool m) { marker_ = m; }
void RtpPacket::setPayloadType(uint8_t pt) { payloadType_ = pt & 0x7F; }
void RtpPacket::setSequenceNumber(uint16_t seq) { sequenceNumber_ = seq; }
void RtpPacket::setTimestamp(uint32_t ts) { timestamp_ = ts; }
void RtpPacket::setSsrc(uint32_t ssrc) { ssrc_ = ssrc; }

void RtpPacket::setPayload(const std::vector<uint8_t>& data) {
    payload_ = data;
}

void RtpPacket::setPayload(const uint8_t* data, size_t len) {
    payload_.assign(data, data + len);
}

size_t RtpPacket::headerSize() const { return 12; }
size_t RtpPacket::totalSize() const { return 12 + payload_.size(); }

} // namespace crystal
