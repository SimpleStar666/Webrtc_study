#pragma once

#include <vector>
#include <cstdint>
#include <functional>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace crystal {

struct H264EncoderConfig {
    int width = 640;
    int height = 480;
    int fps = 30;
    int bitrateKbps = 1000;
    int gopSize = 30;
};

class H264Encoder {
public:
    using EncodedCallback = std::function<void(const uint8_t* nalData,
                                                size_t nalLen)>;

    explicit H264Encoder(const H264EncoderConfig& config);
    ~H264Encoder();

    bool init();
    void encode(const uint8_t* yuvData, size_t len);
    void onEncoded(EncodedCallback cb);

private:
    void processPacket(AVPacket* pkt);

    H264EncoderConfig config_;
    AVCodecContext* codecCtx_ = nullptr;
    AVFrame* frame_ = nullptr;
    int64_t pts_ = 0;
    EncodedCallback encodedCb_;
};

} // namespace crystal
