#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <thread>
#include <atomic>

struct _snd_pcm;

namespace crystal {

struct AlsaConfig {
    std::string device = "default";
    int sampleRate = 48000;
    int channels = 1;
    int frameSize = 960;
};

class AlsaCapture {
public:
    using AudioCallback = std::function<void(const int16_t* data, size_t samples)>;

    explicit AlsaCapture(const AlsaConfig& config);
    ~AlsaCapture();

    bool open();
    void close();
    void startCapture();
    void stopCapture();

    void onAudio(AudioCallback cb);

private:
    void captureLoop();

    AlsaConfig config_;
    _snd_pcm* pcm_ = nullptr;
    std::atomic<bool> capturing_{false};
    std::thread captureThread_;
    AudioCallback audioCb_;
};

} // namespace crystal
