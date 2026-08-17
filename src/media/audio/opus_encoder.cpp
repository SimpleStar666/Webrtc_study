// ===================================================================================
// opus_encoder.cpp - Opus 音频编码器实现
// ===================================================================================
// 本文件实现了 Opus 编码器的初始化和编码逻辑。
// 编码流程：PCM 数据 → Opus 编码器 → 压缩码流
//
// 【Opus 编码器 API 使用要点】
// 1. opus_encoder_create()：创建编码器，需指定应用类型
//    - OPUS_APPLICATION_VOIP：VoIP 语音通话，优化语音质量
//    - OPUS_APPLICATION_AUDIO：音频/音乐，优化音乐质量
//    - OPUS_APPLICATION_RESTRICTED_LOWDELAY：极低延迟模式
// 2. opus_encoder_ctl()：通过泛型控制接口设置编码参数
// 3. opus_encode()：执行编码，输出可变长度的压缩数据
// ===================================================================================

#include "media/audio/opus_encoder.h"
#include "utils/logger.h"

namespace crystal {

// 构造函数 - 仅保存配置，不创建编码器
// 延迟创建（Lazy Initialization）模式：将资源分配推迟到 init() 调用时，
// 允许在对象构造和初始化之间进行状态检查
OpusEncoder::OpusEncoder(const OpusEncoderConfig& config)
    : config_(config) {}

// 析构函数 - 释放编码器资源
// opus_encoder_destroy 释放编码器内部状态内存，包括：
// - SILK 编码器的线性预测滤波器状态
// - CELT 编码器的 MDCT 变换缓冲区
// - 重采样器状态（如果输入采样率非 48kHz）
OpusEncoder::~OpusEncoder() {
    if (encoder_) opus_encoder_destroy(encoder_);
}

// init - 初始化 Opus 编码器
// 核心流程：
//   1. 调用 opus_encoder_create 创建编码器实例
//   2. 设置码率（OPUS_SET_BITRATE）
//   3. 设置复杂度（OPUS_SET_COMPLEXITY）
//   4. 启用 DTX 不连续传输（OPUS_SET_DTX）
// 返回值：true 成功，false 失败
bool OpusEncoder::init() {
    int error;

    // 创建 Opus 编码器实例
    // 参数说明：
    //   - sampleRate：输入音频采样率，Opus 内部统一重采样到 48kHz 处理
    //   - channels：声道数（1=单声道，2=立体声）
    //   - OPUS_APPLICATION_VOIP：应用类型设为 VoIP 语音通话
    //     此模式下编码器会优先使用 SILK 语音编码模式，优化语音质量，
    //     并启用语音活动检测（VAD）等语音专用特性
    //   - &error：错误码输出参数，用于诊断创建失败原因
    encoder_ = opus_encoder_create(config_.sampleRate, config_.channels,
                                    OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK || !encoder_) {
        Logger::error("Failed to create Opus encoder: {}", opus_strerror(error));
        return false;
    }

    // 设置目标码率
    // OPUS_SET_BITRATE 控制编码器输出的平均比特率。
    // 码率选择指南：
    //   6-8 kbps   ：窄带语音（仅语音可懂度）
    //   12-16 kbps ：宽带语音（电话质量）
    //   24-32 kbps ：超宽带语音（高质量语音）
    //   64 kbps    ：全频带语音（接近透明质量，本项目默认值）
    //   96-128 kbps：音乐/混合内容
    //   256+ kbps  ：透明音乐质量
    // 注意：Opus 是可变码率（VBR）编码器，实际码率会围绕目标值波动
    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(config_.bitrateKbps * 1000));

    // 设置编码复杂度（0~10）
    // 复杂度影响编码器的计算量，类似 x264 的 preset：
    //   0  ：最低复杂度，最快编码，质量最低
    //   5  ：中等复杂度，质量和速度的平衡点（推荐默认值）
    //   10 ：最高复杂度，最慢编码，质量最高
    // 复杂度主要影响：
    //   - SILK 模式：影响 LPC 阶数和量化精度
    //   - CELT 模式：影响频带数量和噪声填充策略
    // 在实时通话中，复杂度 5 通常是最佳选择，避免编码耗时导致延迟
    opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(5));

    // 启用 DTX（Discontinuous Transmission，不连续传输）
    // DTX 是语音通话中非常重要的带宽优化技术：
    //   - 当检测到静音/背景噪声时，编码器输出极低码率的"舒适噪声"帧
    //   - 舒适噪声帧仅约 1-2 字节，而正常语音帧约 40-80 字节
    //   - 接收端解码舒适噪声帧后生成类似背景噪声的信号，避免完全静音的突兀感
    //   - 典型语音通话中，约 50% 的时间是静音，DTX 可节省约 50% 带宽
    // 参数：1=启用，0=禁用
    opus_encoder_ctl(encoder_, OPUS_SET_DTX(1));

    Logger::info("Opus encoder initialized: {}Hz {}ch @ {}kbps",
                 config_.sampleRate, config_.channels, config_.bitrateKbps);
    return true;
}

// encode - 编码一帧 PCM 音频数据
// 核心流程：
//   1. 检查编码器是否已初始化
//   2. 分配输出缓冲区（Opus 单帧最大约 4000 字节）
//   3. 调用 opus_encode 执行编码
//   4. 调整输出缓冲区大小为实际编码长度
// 参数：
//   pcmData  - 输入 PCM 数据，16 位有符号整数，交错排列
//   frameSize - 每帧每声道的采样点数
// 返回值：编码后的 Opus 码流，失败时返回空 vector
std::vector<uint8_t> OpusEncoder::encode(const int16_t* pcmData, int frameSize) {
    if (!encoder_) return {};

    // 分配输出缓冲区
    // Opus 编码后的单帧数据最大长度约为 4000 字节（高码率立体声场景）。
    // 实际编码长度通常远小于此值（64kbps 单声道 20ms 帧约 160 字节），
    // 编码完成后会 resize 到实际长度以节省内存
    std::vector<uint8_t> output(4000);

    // 调用 opus_encode 执行编码
    // 参数说明：
    //   - encoder_：编码器状态指针
    //   - pcmData：输入 PCM 数据，格式为 opus_int16（16位有符号整数）
    //   - frameSize：每帧每声道的采样点数（如 960 = 48kHz × 20ms）
    //   - output.data()：输出缓冲区指针
    //   - output.size()：输出缓冲区最大容量
    // 返回值：编码后数据的实际字节数，负值表示错误
    int len = opus_encode(encoder_, pcmData, frameSize,
                          output.data(), static_cast<opus_int32>(output.size()));
    if (len < 0) {
        Logger::warn("Opus encode failed: {}", opus_strerror(len));
        return {};
    }

    // 将输出缓冲区调整为实际编码长度
    // Opus 是可变码率编码器，每帧输出长度不同：
    //   - 语音活动帧：根据信号复杂度动态调整，约 40~160 字节
    //   - DTX 舒适噪声帧：仅 1~2 字节
    //   - 静音帧：可能输出空帧
    output.resize(len);
    return output;
}

} // namespace crystal
