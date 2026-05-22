// ===================================================================================
// opus_decoder.cpp - Opus 音频解码器实现
// ===================================================================================
// 本文件实现了 Opus 解码器的初始化和解码逻辑。
// 解码流程：Opus 码流 → 解码器 → PCM 数据
//
// 【Opus 解码器 API 使用要点】
// 1. opus_decoder_create()：创建解码器，仅需采样率和声道数
// 2. opus_decode()：执行解码，码流中已包含编码模式、帧大小等元信息
// 3. 解码器支持 FEC（Forward Error Correction，前向纠错）：
//    当启用 FEC 时，编码器会在当前帧中嵌入前一帧的冗余信息，
//    解码器可以在前一帧丢失时恢复部分数据
// ===================================================================================

#include "media/audio/opus_decoder.h"
#include "utils/logger.h"

namespace crystal {

// 构造函数 - 保存解码参数
// 解码器比编码器简单，仅需采样率和声道数两个参数，
// 因为 Opus 码流是自描述的（self-describing），
// 码流头部包含编码模式、帧大小、带宽等信息
OpusDecoder::OpusDecoder(int sampleRate, int channels)
    : sampleRate_(sampleRate), channels_(channels) {}

// 析构函数 - 释放解码器资源
// opus_decoder_destroy 释放解码器内部状态内存，包括：
// - SILK 解码器的 LPC 合成滤波器状态
// - CELT 解码器的逆 MDCT 变换缓冲区
// - PLC（丢包隐藏）所需的历史帧信息
OpusDecoder::~OpusDecoder() {
    if (decoder_) opus_decoder_destroy(decoder_);
}

// init - 初始化 Opus 解码器
// 核心流程：调用 opus_decoder_create 创建解码器实例
// 返回值：true 成功，false 失败
bool OpusDecoder::init() {
    int error;

    // 创建 Opus 解码器实例
    // 参数说明：
    //   - sampleRate_：输出音频采样率，Opus 会将内部 48kHz 数据重采样到指定采样率
    //   - channels_：输出声道数（1=单声道，2=立体声）
    //     注意：即使码流是立体声，也可以指定单声道输出，解码器会自动混音
    //   - &error：错误码输出参数
    decoder_ = opus_decoder_create(sampleRate_, channels_, &error);
    if (error != OPUS_OK || !decoder_) {
        Logger::error("Failed to create Opus decoder: {}", opus_strerror(error));
        return false;
    }

    Logger::info("Opus decoder initialized: {}Hz {}ch", sampleRate_, channels_);
    return true;
}

// decode - 解码一帧 Opus 码流数据
// 核心流程：
//   1. 检查解码器是否已初始化
//   2. 分配输出缓冲区（frameSize × channels 个采样点）
//   3. 调用 opus_decode 执行解码
//   4. 调整输出缓冲区大小为实际解码采样数
// 参数：
//   opusData  - Opus 码流数据指针
//   len       - 码流长度（字节）
//   frameSize - 期望输出的每帧每声道采样点数
// 返回值：解码后的 PCM 数据，失败时返回空 vector
std::vector<int16_t> OpusDecoder::decode(const uint8_t* opusData, size_t len,
                                           int frameSize) {
    if (!decoder_) return {};

    // 分配输出缓冲区
    // 大小 = 每声道采样点数 × 声道数
    // 例如：960 × 1 = 960 个 int16_t（单声道 20ms 帧）
    //       960 × 2 = 1920 个 int16_t（立体声 20ms 帧，左右声道交错）
    std::vector<int16_t> output(frameSize * channels_);

    // 调用 opus_decode 执行解码
    // 参数说明：
    //   - decoder_：解码器状态指针
    //   - opusData：输入的 Opus 码流数据
    //   - len：码流数据长度（字节）
    //   - output.data()：输出 PCM 数据缓冲区
    //   - frameSize：期望输出的每声道采样点数
    //   - 0（最后一个参数 decode_fec）：
    //     0 = 正常解码当前帧
    //     1 = 解码嵌入在当前帧中的 FEC 冗余数据（用于恢复前一丢失帧）
    //     FEC 是 Opus 的抗丢包机制，编码端在当前帧中嵌入前一帧的低码率副本，
    //     当检测到前一帧丢失时，解码端可以从当前帧中提取 FEC 数据恢复
    int samples = opus_decode(decoder_, opusData, static_cast<opus_int32>(len),
                               output.data(), frameSize, 0);
    if (samples < 0) {
        Logger::warn("Opus decode failed: {}", opus_strerror(samples));
        return {};
    }

    // 调整输出缓冲区为实际解码的采样数
    // samples 是每声道的采样点数，总数据量 = samples × channels
    // 实际解码的采样数可能与 frameSize 不同，例如：
    //   - DTX 舒适噪声帧可能解码出较短的数据
    //   - 某些帧大小配置下解码器可能输出不同数量的采样点
    output.resize(samples * channels_);
    return output;
}

} // namespace crystal
