#pragma once

#include <cstdint>
#include <vector>
#include <cstddef>

namespace crystal {

class RtpPacket {
public:
    RtpPacket();
    ~RtpPacket() = default;

    bool parse(const uint8_t* data, size_t size);
    std::vector<uint8_t> serialize() const;

    uint8_t version() const;
    bool padding() const;
    bool extension() const;
    uint8_t csrcCount() const;
    bool marker() const;
    uint8_t payloadType() const;
    uint16_t sequenceNumber() const;
    uint32_t timestamp() const;
    uint32_t ssrc() const;
    const std::vector<uint8_t>& payload() const;

    void setMarker(bool m);
    void setPayloadType(uint8_t pt);
    void setSequenceNumber(uint16_t seq);
    void setTimestamp(uint32_t ts);
    void setSsrc(uint32_t ssrc);
    void setPayload(const std::vector<uint8_t>& data);
    void setPayload(const uint8_t* data, size_t len);

    size_t headerSize() const;
    size_t totalSize() const;

private:
    bool marker_ = false;
    uint8_t payloadType_ = 0;
    uint16_t sequenceNumber_ = 0;
    uint32_t timestamp_ = 0;
    uint32_t ssrc_ = 0;
    std::vector<uint8_t> payload_;
};

} // namespace crystal
