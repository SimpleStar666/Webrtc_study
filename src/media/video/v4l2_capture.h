#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <thread>
#include <atomic>

namespace crystal {

struct V4L2Config {
    std::string device = "/dev/video0";
    int width = 640;
    int height = 480;
    int fps = 30;
};

class V4L2Capture {
public:
    using FrameCallback = std::function<void(const uint8_t* yuvData,
                                             size_t len)>;

    explicit V4L2Capture(const V4L2Config& config);
    ~V4L2Capture();

    bool open();
    void close();
    void startCapture();
    void stopCapture();

    void onFrame(FrameCallback cb);

private:
    void captureLoop();

    V4L2Config config_;
    int fd_ = -1;
    std::atomic<bool> capturing_{false};
    std::thread captureThread_;
    FrameCallback frameCb_;
    std::vector<uint8_t> buffer_;
};

} // namespace crystal
