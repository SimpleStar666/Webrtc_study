#include "media/audio/opus_encoder.h"
#include "utils/logger.h"

namespace crystal {

OpusEncoder::OpusEncoder(const OpusEncoderConfig& config)
    : config_(config) {}

OpusEncoder::~OpusEncoder() {
    if (encoder_) opus_encoder_destroy(encoder_);
}

bool OpusEncoder::init() {
    int error;
    encoder_ = opus_encoder_create(config_.sampleRate, config_.channels,
                                    OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK || !encoder_) {
        Logger::error("Failed to create Opus encoder: {}", opus_strerror(error));
        return false;
    }

    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(config_.bitrateKbps * 1000));
    opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(encoder_, OPUS_SET_DTX(1));

    Logger::info("Opus encoder initialized: {}Hz {}ch @ {}kbps",
                 config_.sampleRate, config_.channels, config_.bitrateKbps);
    return true;
}

std::vector<uint8_t> OpusEncoder::encode(const int16_t* pcmData, int frameSize) {
    if (!encoder_) return {};

    std::vector<uint8_t> output(4000);
    int len = opus_encode(encoder_, pcmData, frameSize,
                          output.data(), static_cast<opus_int32>(output.size()));
    if (len < 0) {
        Logger::warn("Opus encode failed: {}", opus_strerror(len));
        return {};
    }

    output.resize(len);
    return output;
}

} // namespace crystal
