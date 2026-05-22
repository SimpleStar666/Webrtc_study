#include "media/audio/sdl_audio_player.h"
#include "utils/logger.h"
#include <SDL2/SDL.h>
#include <cstring>

namespace crystal {

SDLAudioPlayer::SDLAudioPlayer(int sampleRate, int channels)
    : sampleRate_(sampleRate), channels_(channels) {}

SDLAudioPlayer::~SDLAudioPlayer() {
    stop();
}

bool SDLAudioPlayer::init() {
    SDL_AudioSpec spec;
    spec.freq = sampleRate_;
    spec.format = AUDIO_S16LSB;
    spec.channels = channels_;
    spec.samples = 960;
    spec.callback = audioCallback;
    spec.userdata = this;

    if (SDL_OpenAudio(&spec, nullptr) < 0) {
        Logger::error("SDL audio open failed: {}", SDL_GetError());
        return false;
    }

    SDL_PauseAudio(0);
    Logger::info("SDL audio player initialized: {}Hz {}ch", sampleRate_, channels_);
    return true;
}

void SDLAudioPlayer::play(const int16_t* data, size_t samples) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < samples * channels_; i++) {
        buffer_.push(data[i]);
    }
}

void SDLAudioPlayer::stop() {
    SDL_CloseAudio();
}

void SDLAudioPlayer::audioCallback(void* userdata, uint8_t* stream, int len) {
    auto* self = static_cast<SDLAudioPlayer*>(userdata);
    self->fillBuffer(stream, len);
}

void SDLAudioPlayer::fillBuffer(uint8_t* stream, int len) {
    std::lock_guard<std::mutex> lock(mutex_);
    int16_t* out = reinterpret_cast<int16_t*>(stream);
    int samples = len / 2;

    for (int i = 0; i < samples; i++) {
        if (!buffer_.empty()) {
            out[i] = buffer_.front();
            buffer_.pop();
        } else {
            out[i] = 0;
        }
    }
}

} // namespace crystal
