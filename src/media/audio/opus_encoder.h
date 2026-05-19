#pragma once

#include <opus/opus.h>
#include <vector>
#include <cstdint>
#include <functional>

namespace crystal {

struct OpusEncoderConfig {
    int sampleRate = 48000;
    int channels = 1;
    int bitrateKbps = 64;
    int frameSize = 960;
};

class OpusEncoder {
public:
    explicit OpusEncoder(const OpusEncoderConfig& config);
    ~OpusEncoder();

    bool init();
    std::vector<uint8_t> encode(const int16_t* pcmData, int frameSize);
    int frameSize() const { return config_.frameSize; }

private:
    OpusEncoderConfig config_;
    ::OpusEncoder* encoder_ = nullptr;
};

} // namespace crystal
