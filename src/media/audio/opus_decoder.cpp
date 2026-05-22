#include "media/audio/opus_decoder.h"
#include "utils/logger.h"

namespace crystal {

OpusDecoder::OpusDecoder(int sampleRate, int channels)
    : sampleRate_(sampleRate), channels_(channels) {}

OpusDecoder::~OpusDecoder() {
    if (decoder_) opus_decoder_destroy(decoder_);
}

bool OpusDecoder::init() {
    int error;
    decoder_ = opus_decoder_create(sampleRate_, channels_, &error);
    if (error != OPUS_OK || !decoder_) {
        Logger::error("Failed to create Opus decoder: {}", opus_strerror(error));
        return false;
    }

    Logger::info("Opus decoder initialized: {}Hz {}ch", sampleRate_, channels_);
    return true;
}

std::vector<int16_t> OpusDecoder::decode(const uint8_t* opusData, size_t len,
                                           int frameSize) {
    if (!decoder_) return {};

    std::vector<int16_t> output(frameSize * channels_);
    int samples = opus_decode(decoder_, opusData, static_cast<opus_int32>(len),
                               output.data(), frameSize, 0);
    if (samples < 0) {
        Logger::warn("Opus decode failed: {}", opus_strerror(samples));
        return {};
    }

    output.resize(samples * channels_);
    return output;
}

} // namespace crystal
