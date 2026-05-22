#include "media/video/h264_decoder.h"
#include "utils/logger.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}
#include <cstring>

namespace crystal {

H264Decoder::H264Decoder() = default;

H264Decoder::~H264Decoder() {
    if (pkt_) av_packet_free(&pkt_);
    if (frame_) av_frame_free(&frame_);
    if (codecCtx_) avcodec_free_context(&codecCtx_);
}

bool H264Decoder::init() {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        Logger::error("H264 decoder not found");
        return false;
    }

    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        Logger::error("Failed to allocate H264 decoder context");
        return false;
    }

    codecCtx_->flags2 |= AV_CODEC_FLAG2_FAST;

    int ret = avcodec_open2(codecCtx_, codec, nullptr);
    if (ret < 0) {
        Logger::error("Failed to open H264 decoder: {}", ret);
        return false;
    }

    frame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    initialized_ = true;

    Logger::info("H264 decoder initialized");
    return true;
}

void H264Decoder::decode(const uint8_t* nalData, size_t nalLen) {
    if (!initialized_) return;

    std::vector<uint8_t> annexB;
    annexB.reserve(4 + nalLen);
    annexB.push_back(0x00);
    annexB.push_back(0x00);
    annexB.push_back(0x00);
    annexB.push_back(0x01);
    annexB.insert(annexB.end(), nalData, nalData + nalLen);

    pkt_->data = annexB.data();
    pkt_->size = static_cast<int>(annexB.size());

    int ret = avcodec_send_packet(codecCtx_, pkt_);
    if (ret < 0) {
        Logger::debug("Failed to send packet to decoder: {}", ret);
        return;
    }

    while (avcodec_receive_frame(codecCtx_, frame_) == 0) {
        if (decodedCb_) {
            decodedCb_(frame_->data[0], frame_->width, frame_->height);
        }
    }
}

void H264Decoder::onDecoded(DecodedCallback cb) {
    decodedCb_ = std::move(cb);
}

} // namespace crystal
