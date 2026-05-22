#pragma once

#include <opus/opus.h>
#include <vector>
#include <cstdint>

namespace crystal {

class OpusDecoder {
public:
    OpusDecoder(int sampleRate = 48000, int channels = 1);
    ~OpusDecoder();

    bool init();
    std::vector<int16_t> decode(const uint8_t* opusData, size_t len,
                                 int frameSize = 960);

private:
    int sampleRate_;
    int channels_;
    ::OpusDecoder* decoder_ = nullptr;
};

} // namespace crystal
