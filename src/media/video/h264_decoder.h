#pragma once

#include <vector>
#include <cstdint>
#include <functional>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace crystal {

class H264Decoder {
public:
    using DecodedCallback = std::function<void(const uint8_t* yuvData,
                                                int width, int height)>;

    H264Decoder();
    ~H264Decoder();

    bool init();
    void decode(const uint8_t* nalData, size_t nalLen);
    void onDecoded(DecodedCallback cb);

private:
    AVCodecContext* codecCtx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    bool initialized_ = false;
    DecodedCallback decodedCb_;
};

} // namespace crystal
