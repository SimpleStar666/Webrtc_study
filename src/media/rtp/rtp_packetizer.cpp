#include "media/rtp/rtp_packetizer.h"
#include "utils/logger.h"

namespace crystal {

RtpPacketizer::RtpPacketizer(uint8_t payloadType, uint32_t clockRate,
                             uint32_t ssrc, uint16_t startSeqNum,
                             size_t maxPacketSize)
    : payloadType_(payloadType), clockRate_(clockRate), ssrc_(ssrc),
      sequenceNumber_(startSeqNum), maxPacketSize_(maxPacketSize) {}

std::vector<RtpPacket> RtpPacketizer::packetizeH264(
    const std::vector<uint8_t>& nalUnit, uint32_t timestamp) {

    std::vector<RtpPacket> result;

    if (nalUnit.size() <= maxPacketSize_) {
        RtpPacket pkt;
        pkt.setPayloadType(payloadType_);
        pkt.setSequenceNumber(sequenceNumber_++);
        pkt.setTimestamp(timestamp);
        pkt.setSsrc(ssrc_);
        pkt.setMarker(true);
        pkt.setPayload(nalUnit);
        result.push_back(std::move(pkt));
        return result;
    }

    uint8_t nalRefIdc = (nalUnit[0] & 0x60) >> 5;
    uint8_t nalType = nalUnit[0] & 0x1F;

    const uint8_t* nalData = nalUnit.data() + 1;
    size_t nalDataLen = nalUnit.size() - 1;
    size_t maxFragLen = maxPacketSize_ - 2;

    size_t offset = 0;
    while (offset < nalDataLen) {
        size_t fragLen = std::min(maxFragLen, nalDataLen - offset);
        bool isFirst = (offset == 0);
        bool isLast = (offset + fragLen >= nalDataLen);

        std::vector<uint8_t> fuPayload;
        fuPayload.reserve(2 + fragLen);

        uint8_t fuIndicator = (nalRefIdc << 5) | 28;
        fuPayload.push_back(fuIndicator);

        uint8_t fuHeader = (nalType & 0x1F);
        if (isFirst) fuHeader |= 0x80;
        if (isLast) fuHeader |= 0x40;
        fuPayload.push_back(fuHeader);

        fuPayload.insert(fuPayload.end(), nalData + offset,
                         nalData + offset + fragLen);

        RtpPacket pkt;
        pkt.setPayloadType(payloadType_);
        pkt.setSequenceNumber(sequenceNumber_++);
        pkt.setTimestamp(timestamp);
        pkt.setSsrc(ssrc_);
        pkt.setMarker(isLast);
        pkt.setPayload(fuPayload);
        result.push_back(std::move(pkt));

        offset += fragLen;
    }

    Logger::debug("FU-A fragmented NAL (type={} size={}) into {} RTP packets",
                  static_cast<int>(nalType), nalUnit.size(), result.size());
    return result;
}

std::vector<RtpPacket> RtpPacketizer::packetizeOpus(
    const std::vector<uint8_t>& opusFrame, uint32_t timestamp) {

    RtpPacket pkt;
    pkt.setPayloadType(payloadType_);
    pkt.setSequenceNumber(sequenceNumber_++);
    pkt.setTimestamp(timestamp);
    pkt.setSsrc(ssrc_);
    pkt.setMarker(true);
    pkt.setPayload(opusFrame);

    std::vector<RtpPacket> result;
    result.push_back(std::move(pkt));
    return result;
}

} // namespace crystal
