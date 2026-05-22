// ===================================================================================
// opus_decoder.h - Opus 音频解码器头文件
// ===================================================================================
// 本文件定义了 CrystalRTC 项目中的 Opus 音频解码器组件。
//
// 【在 WebRTC 系统中的角色】
// 在 WebRTC 音频接收流水线中，解码器位于"接收 → Jitter Buffer → 解码 → 播放"链路的解码环节。
// 从网络收到的 RTP 包经过 Jitter Buffer（抖动缓冲）排序和去抖后，
// 提取 Opus 码流送入解码器，还原为 PCM 音频数据，再交给音频播放设备输出。
//
// 【Opus 解码器与编码器的关系】
// Opus 解码器比编码器简单得多，因为：
//   1. 解码器不需要进行信号分析、模式选择、码率控制等复杂决策
//   2. 解码器只需按照码流中嵌入的模式信息（SILK/CELT/Hybrid）执行逆变换
//   3. 解码器的计算量约为编码器的 1/5~1/10
//   4. 解码器不需要配置码率、复杂度等参数，所有信息已在码流中
//
// 【PLC（Packet Loss Concealment，丢包隐藏）】
// Opus 解码器内置了丢包隐藏功能。当网络丢包导致某些帧缺失时，
// 解码器可以根据前一帧的信息插值生成近似信号，掩盖丢包造成的影响。
// 这是通过 opus_decode() 的最后一个参数（decode_fec）控制的。
// ===================================================================================

#pragma once

#include <opus/opus.h>
#include <vector>
#include <cstdint>

namespace crystal {

// OpusDecoder - Opus 音频解码器类
// 职责：将 Opus 压缩码流解码为原始 PCM 音频数据。
// 设计思路：
//   - 与 OpusEncoder 对称设计，封装 libopus 的解码 API
//   - 解码器参数较少，仅需采样率和声道数（码率等信息已嵌入码流）
//   - 解码结果以 vector<int16_t> 返回，大小由帧大小和声道数决定
class OpusDecoder {
public:
    // 构造函数 - 指定音频参数创建解码器对象
    // 参数：
    //   sampleRate - 采样率（Hz），默认 48000，应与编码端一致
    //   channels   - 声道数，默认 1（单声道），应与编码端一致
    // 注意：与编码器不同，解码器不需要码率、复杂度等参数，
    //       因为这些信息已嵌入 Opus 码流中，解码器会自动解析
    OpusDecoder(int sampleRate = 48000, int channels = 1);

    // 析构函数 - 释放 Opus 解码器资源
    // 自动调用 opus_decoder_destroy 释放 libopus 分配的解码器状态内存
    ~OpusDecoder();

    // init - 初始化解码器
    // 创建 Opus 解码器实例。
    // 必须在调用 decode() 之前调用。
    // 返回值：true 初始化成功，false 初始化失败
    bool init();

    // decode - 解码一帧 Opus 码流数据
    // 将 Opus 压缩数据解码为 PCM 音频数据。
    // 参数：
    //   opusData  - 输入的 Opus 码流数据指针
    //   len       - 码流数据长度（字节）
    //   frameSize - 期望输出的每帧每声道采样点数，默认 960（48kHz 下 20ms）
    //               解码器会尝试输出指定数量的采样点，如果码流中的帧大小
    //               与 frameSize 不匹配，解码器会进行相应的帧拼接或拆分
    // 返回值：解码后的 PCM 数据（int16_t），交错排列。
    //         若解码失败返回空 vector。
    std::vector<int16_t> decode(const uint8_t* opusData, size_t len,
                                 int frameSize = 960);

private:
    int sampleRate_;              // 采样率（Hz），解码器按此采样率输出 PCM 数据
    int channels_;                // 声道数，决定输出数据的交错方式
    ::OpusDecoder* decoder_ = nullptr;  // libopus 解码器状态指针
                                  // 这是由 opus_decoder_create() 分配的不透明结构体，
                                  // 包含解码器的全部内部状态（上一帧的合成信号、
                                  // PLC 丢包隐藏所需的帧间信息等）
};

} // namespace crystal
