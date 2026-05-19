#include "media/audio/alsa_capture.h"
#include "utils/logger.h"
#include <alsa/asoundlib.h>

namespace crystal {

AlsaCapture::AlsaCapture(const AlsaConfig& config) : config_(config) {}

AlsaCapture::~AlsaCapture() {
    close();
}

bool AlsaCapture::open() {
    int err = snd_pcm_open(&pcm_, config_.device.c_str(),
                           SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        Logger::error("ALSA open failed: {}", snd_strerror(err));
        return false;
    }

    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pcm_, params);

    snd_pcm_hw_params_set_access(pcm_, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm_, params, config_.channels);

    unsigned int rate = config_.sampleRate;
    snd_pcm_hw_params_set_rate_near(pcm_, params, &rate, nullptr);

    snd_pcm_uframes_t frames = config_.frameSize;
    snd_pcm_hw_params_set_period_size_near(pcm_, params, &frames, nullptr);

    err = snd_pcm_hw_params(pcm_, params);
    if (err < 0) {
        Logger::error("ALSA hw_params failed: {}", snd_strerror(err));
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return false;
    }

    Logger::info("ALSA capture opened: {}Hz {}ch on {}",
                 config_.sampleRate, config_.channels, config_.device);
    return true;
}

void AlsaCapture::close() {
    stopCapture();
    if (pcm_) {
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
}

void AlsaCapture::startCapture() {
    if (!pcm_) return;
    capturing_ = true;
    captureThread_ = std::thread(&AlsaCapture::captureLoop, this);
    Logger::info("ALSA capture started");
}

void AlsaCapture::stopCapture() {
    capturing_ = false;
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
}

void AlsaCapture::captureLoop() {
    std::vector<int16_t> buffer(config_.frameSize * config_.channels);

    while (capturing_) {
        int frames = snd_pcm_readi(pcm_, buffer.data(), config_.frameSize);
        if (frames < 0) {
            frames = snd_pcm_recover(pcm_, frames, 0);
            if (frames < 0) {
                Logger::warn("ALSA read failed: {}", snd_strerror(frames));
                break;
            }
            continue;
        }

        if (audioCb_) {
            audioCb_(buffer.data(), static_cast<size_t>(frames));
        }
    }
}

void AlsaCapture::onAudio(AudioCallback cb) {
    audioCb_ = std::move(cb);
}

} // namespace crystal
