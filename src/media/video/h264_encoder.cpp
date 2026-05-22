#include "media/video/h264_encoder.h"
#include "utils/logger.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
}

namespace crystal {

H264Encoder::H264Encoder(const H264EncoderConfig& config)
    : config_(config) {}

H264Encoder::~H264Encoder() {
    if (frame_) av_frame_free(&frame_);
    if (codecCtx_) avcodec_free_context(&codecCtx_);
}

bool H264Encoder::init() {
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    }
    if (!codec) {
        Logger::error("H264 encoder not found");
        return false;
    }

    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        Logger::error("Failed to allocate H264 encoder context");
        return false;
    }

    codecCtx_->bit_rate = config_.bitrateKbps * 1000;
    codecCtx_->width = config_.width;
    codecCtx_->height = config_.height;
    codecCtx_->time_base = {1, config_.fps};
    codecCtx_->framerate = {config_.fps, 1};
    codecCtx_->gop_size = config_.gopSize;
    codecCtx_->max_b_frames = 0;
    codecCtx_->pix_fmt = AV_PIX_FMT_YUV420P;
    codecCtx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    av_opt_set(codecCtx_->priv_data, "preset", "ultrafast", 0);
    av_opt_set(codecCtx_->priv_data, "tune", "zerolatency", 0);
    av_opt_set(codecCtx_->priv_data, "profile", "baseline", 0);

    int ret = avcodec_open2(codecCtx_, codec, nullptr);
    if (ret < 0) {
        Logger::error("Failed to open H264 encoder: {}", ret);
        return false;
    }

    frame_ = av_frame_alloc();
    frame_->format = AV_PIX_FMT_YUV420P;
    frame_->width = config_.width;
    frame_->height = config_.height;
    av_frame_get_buffer(frame_, 0);

    Logger::info("H264 encoder initialized: {}x{} @ {}kbps",
                 config_.width, config_.height, config_.bitrateKbps);
    return true;
}

void H264Encoder::encode(const uint8_t* yuvData, size_t len) {
    int ySize = config_.width * config_.height;
    int uvSize = ySize / 4;
    int expectedLen = ySize + 2 * uvSize;

    if (static_cast<int>(len) < expectedLen) {
        Logger::warn("YUV data too short: {} < {}", len, expectedLen);
        return;
    }

    frame_->data[0] = const_cast<uint8_t*>(yuvData);
    frame_->data[1] = const_cast<uint8_t*>(yuvData + ySize);
    frame_->data[2] = const_cast<uint8_t*>(yuvData + ySize + uvSize);
    frame_->linesize[0] = config_.width;
    frame_->linesize[1] = config_.width / 2;
    frame_->linesize[2] = config_.width / 2;
    frame_->pts = pts_++;

    int ret = avcodec_send_frame(codecCtx_, frame_);
    if (ret < 0) {
        Logger::warn("Failed to send frame to encoder: {}", ret);
        return;
    }

    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(codecCtx_, pkt) == 0) {
        processPacket(pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
}

void H264Encoder::processPacket(AVPacket* pkt) {
    const uint8_t* data = pkt->data;
    int size = pkt->size;
    int offset = 0;

    while (offset < size) {
        if (offset + 4 > size) break;

        int nalSize = (data[offset] << 24) | (data[offset + 1] << 16) |
                      (data[offset + 2] << 8) | data[offset + 3];
        offset += 4;

        if (offset + nalSize > size) break;

        if (encodedCb_) {
            encodedCb_(data + offset, nalSize);
        }
        offset += nalSize;
    }
}

void H264Encoder::onEncoded(EncodedCallback cb) {
    encodedCb_ = std::move(cb);
}

} // namespace crystal
