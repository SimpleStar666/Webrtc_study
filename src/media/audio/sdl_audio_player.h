#pragma once

#include <cstdint>
#include <vector>
#include <mutex>
#include <queue>

namespace crystal {

class SDLAudioPlayer {
public:
    SDLAudioPlayer(int sampleRate = 48000, int channels = 1);
    ~SDLAudioPlayer();

    bool init();
    void play(const int16_t* data, size_t samples);
    void stop();

private:
    static void audioCallback(void* userdata, uint8_t* stream, int len);
    void fillBuffer(uint8_t* stream, int len);

    int sampleRate_;
    int channels_;
    std::mutex mutex_;
    std::queue<int16_t> buffer_;
};

} // namespace crystal
