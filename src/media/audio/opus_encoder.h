// ===================================================================================
// opus_encoder.h - Opus 音频编码器头文件
// ===================================================================================
// 本文件定义了 CrystalRTC 项目中的 Opus 音频编码器组件。
//
// 【在 WebRTC 系统中的角色】
// 在 WebRTC 音频处理流水线中，编码器位于"采集 → 前处理 → 编码 → 传输"链路的编码环节。
// 音频采集设备（如 ALSA）获取原始 PCM 数据后，经 Opus 编码器压缩为码流，
// 再通过 RTP 协议发送到远端。Opus 是 WebRTC 的强制编解码器（Mandatory Codec），
// 所有符合标准的 WebRTC 实现都必须支持 Opus。
//
// 【Opus 编解码核心概念】
// Opus 是一个混合编码器，内部融合了两种编码技术：
//   1. SILK 模式：基于线性预测（LPC）的语音编码，适合低码率语音（6~40 kbps），
//      利用语音信号的准周期性进行高效压缩，类似 Skype 使用的 SVOPC 编码器。
//   2. CELT 模式：基于改进离散余弦变换（MDCT）的音乐编码，适合中高码率（40~510 kbps），
//      类似 MP3/AAC 的变换编码思路，对音乐和混合信号表现优秀。
//   3. Hybrid 混合模式：在中等码率（约 24~32 kbps）下，同时使用 SILK 和 CELT，
//      SILK 处理低频语音成分，CELT 处理高频成分，兼顾语音清晰度和频带宽度。
// Opus 编码器会根据信号特征和码率自动选择最优模式，无需应用层干预。
//
// 【音频帧大小与延迟的关系】
// Opus 支持的帧大小：2.5ms, 5ms, 10ms, 20ms, 40ms, 60ms（对应 48kHz 下 120~2880 采样点）。
// 帧大小直接影响算法延迟：帧越大压缩效率越高，但延迟也越大。
// WebRTC 语音通话通常使用 20ms 帧（960 采样点 @48kHz），这是延迟与质量的最佳平衡点。
// ===================================================================================

#pragma once

#include <opus/opus.h>
#include <vector>
#include <cstdint>
#include <functional>

namespace crystal {

// OpusEncoderConfig - Opus 编码器配置结构体
// 用于在创建编码器时指定音频参数，这些参数直接影响编码质量和网络带宽占用。
struct OpusEncoderConfig {
    int sampleRate = 48000;   // 采样率（Hz）。Opus 内部始终工作在 48kHz，
                              // 但编码器支持 8/12/16/24/48 kHz 输入，内部会自动重采样。
                              // WebRTC 标准要求 Opus 使用 48kHz 采样率。

    int channels = 1;         // 声道数。1=单声道（语音通话默认），2=立体声。
                              // 单声道语音通话仅需一半带宽，立体声用于音乐共享等场景。

    int bitrateKbps = 64;     // 目标码率（kbps）。Opus 支持范围 6~510 kbps。
                              // 语音通话推荐值：6~8 kbps（极窄带）到 32~64 kbps（宽带语音）。
                              // 64 kbps 是 WebRTC 语音通话的常用默认值，提供高质量语音。

    int frameSize = 960;      // 每帧采样点数。960 对应 48kHz 下 20ms 的帧大小。
                              // 这是 WebRTC 语音通话的标准帧大小。
                              // 计算方式：frameSize = sampleRate × frameDuration
                              // 例如：48000 × 0.020 = 960
};

// OpusEncoder - Opus 音频编码器类
// 职责：将原始 PCM 音频数据编码为 Opus 压缩码流。
// 设计思路：
//   - 封装 libopus 的 C API 为 C++ 类，提供 RAII 风格的生命周期管理
//   - 编码器在 init() 中创建，析构时自动释放，避免资源泄漏
//   - 编码结果以 vector<uint8_t> 返回，大小动态变化（Opus 是可变码率编码器）
class OpusEncoder {
public:
    // 构造函数 - 使用指定配置创建编码器对象
    // 注意：构造函数仅保存配置，实际编码器创建在 init() 中完成
    // 参数：config - 编码器配置，包含采样率、声道数、码率、帧大小
    explicit OpusEncoder(const OpusEncoderConfig& config);

    // 析构函数 - 释放 Opus 编码器资源
    // 自动调用 opus_encoder_destroy 释放 libopus 分配的编码器状态内存
    ~OpusEncoder();

    // init - 初始化编码器
    // 创建 Opus 编码器实例并设置编码参数（码率、复杂度、DTX 等）。
    // 必须在调用 encode() 之前调用。
    // 返回值：true 初始化成功，false 初始化失败（如参数不合法）
    bool init();

    // encode - 编码一帧 PCM 音频数据
    // 将原始 PCM 采样数据编码为 Opus 压缩数据。
    // 参数：
    //   pcmData  - 输入的 PCM 采样数据指针，格式为 16 位有符号整数（int16_t），
    //              交错排列（多声道时左右声道交替存放）
    //   frameSize - 每帧采样点数（每个声道的采样数），应与配置一致
    //              例如 48kHz 20ms 帧 = 960 采样点
    // 返回值：编码后的 Opus 码流数据，大小可变（Opus 可变码率特性）。
    //         若编码失败返回空 vector。
    std::vector<uint8_t> encode(const int16_t* pcmData, int frameSize);

    // frameSize - 获取配置的帧大小（采样点数）
    // 返回值：每帧采样点数，用于外部模块按正确大小提供 PCM 数据
    int frameSize() const { return config_.frameSize; }

private:
    OpusEncoderConfig config_;   // 编码器配置参数
    ::OpusEncoder* encoder_ = nullptr;  // libopus 编码器状态指针
                                  // 这是由 opus_encoder_create() 分配的不透明结构体，
                                  // 包含编码器的全部内部状态（滤波器历史、量化表等），
                                  // 编码连续帧时需要保持此状态以利用帧间相关性
};

} // namespace crystal
