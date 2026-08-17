// ===================================================================================
// alsa_capture.h - ALSA 音频采集头文件
// ===================================================================================
// 本文件定义了 CrystalRTC 项目中的 ALSA 音频采集组件。
//
// 【在 WebRTC 系统中的角色】
// 在 WebRTC 音频发送流水线中，采集模块位于最前端：
// "采集 → 前处理 → 编码 → 传输"
// ALSA（Advanced Linux Sound Architecture）是 Linux 系统的标准音频接口，
// 本模块通过 ALSA 从麦克风等输入设备采集原始 PCM 音频数据，
// 然后通过回调函数将数据传递给编码器进行压缩。
//
// 【ALSA 核心概念】
// ALSA 是 Linux 内核的音频子系统，提供对声卡硬件的底层访问：
//   1. PCM 设备：PCM（Pulse Code Modulation）设备是 ALSA 的核心抽象，
//      代表一个可以读写音频采样数据的音频流通道。
//   2. Capture vs Playback：PCM 设备分为采集（Capture）和回放（Playback）两种，
//      采集设备从麦克风读取数据，回放设备向扬声器写入数据。
//   3. Period 和 Buffer：ALSA 使用环形缓冲区管理音频数据。
//      - Buffer：整个环形缓冲区的大小
//      - Period：每次中断传递的数据块大小，也称为"片段"
//      当硬件处理完一个 period 的数据后，触发中断通知应用层读写。
//      Period 大小直接影响延迟：period 越小延迟越低，但 CPU 中断频率越高。
//
// 【音频参数说明】
//   - 采样率（Sample Rate）：每秒采样的次数，48kHz 表示每秒 48000 个采样点
//   - 通道数（Channels）：1=单声道，2=立体声
//   - 采样格式（Format）：S16_LE 表示 16 位有符号小端序，每个采样占 2 字节
//   - 帧大小（Frame Size）：每次读取的采样点数（每声道），
//     960 采样点 @48kHz = 20ms 的音频数据
// ===================================================================================

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <thread>
#include <atomic>

// 前向声明 ALSA PCM 结构体，避免在头文件中包含 ALSA 的 C 头文件
// 这是一种常见的 Pimpl（Pointer to Implementation）惯用法，
// 减少头文件依赖，加快编译速度，避免 ALSSA 内部定义污染用户命名空间
struct _snd_pcm;

namespace crystal {

// AlsaConfig - ALSA 采集配置结构体
// 用于指定音频采集的硬件参数，这些参数需要与声卡能力匹配
struct AlsaConfig {
    std::string device = "default";  // ALSA 设备名称
                                     // "default" 表示系统默认音频输入设备
                                     // 其他常见值：
                                     //   "hw:0,0" - 第 0 块声卡的第 0 个子设备（直接硬件访问）
                                     //   "plughw:0,0" - 带插件层的硬件设备（支持格式转换）
                                     //   "pulse" - PulseAudio 虚拟设备
                                     // 直接硬件访问延迟最低，但要求参数完全匹配；
                                     // 插件层会自动转换格式，但增加延迟

    int sampleRate = 48000;   // 采样率（Hz）。48kHz 是 Opus 编码器的内部工作频率，
                              // 使用 48kHz 采集可避免重采样带来的质量损失和额外延迟。
                              // 其他常见采样率：8000（电话）、16000（宽带语音）、44100（CD）

    int channels = 1;         // 声道数。语音通话默认使用单声道，
                              // 单声道数据量仅为立体声的一半，节省编码和传输开销

    int frameSize = 960;      // 每次读取的帧大小（每声道采样点数）
                              // 960 @48kHz = 20ms，与 Opus 编码器的标准帧大小对齐
                              // 这确保每次采集的数据恰好是一帧 Opus 编码的输入
};

// AlsaCapture - ALSA 音频采集类
// 职责：从 ALSA 音频设备持续采集 PCM 音频数据，通过回调函数传递给消费者。
// 设计思路：
//   - 使用独立线程进行音频采集，避免阻塞主线程
//   - 采用回调模式（Observer Pattern），采集到数据后通知消费者
//   - 线程安全：capturing_ 使用 atomic 保证多线程可见性
//   - RAII 管理：析构时自动停止采集并关闭设备
class AlsaCapture {
public:
    // AudioCallback - 音频数据回调类型
    // 当采集到一帧音频数据时触发此回调
    // 参数：
    //   data - PCM 采样数据指针，格式为 int16_t，交错排列
    //   samples - 本次回调提供的采样点数（每声道）
    using AudioCallback = std::function<void(const int16_t* data, size_t samples)>;

    // 构造函数 - 使用指定配置创建采集器对象
    // 参数：config - ALSA 采集配置
    explicit AlsaCapture(const AlsaConfig& config);

    // 析构函数 - 停止采集并关闭 ALSA 设备
    ~AlsaCapture();

    // open - 打开 ALSA 采集设备并配置硬件参数
    // 核心流程：
    //   1. 打开 PCM 设备
    //   2. 分配并初始化硬件参数
    //   3. 设置访问模式、采样格式、声道数、采样率、周期大小
    //   4. 应用硬件参数到设备
    // 返回值：true 成功，false 失败
    bool open();

    // close - 关闭 ALSA 采集设备
    // 先停止采集线程，再关闭 PCM 设备释放资源
    void close();

    // startCapture - 启动音频采集
    // 创建采集线程，在线程中循环读取音频数据
    void startCapture();

    // stopCapture - 停止音频采集
    // 设置停止标志并等待采集线程结束
    void stopCapture();

    // onAudio - 注册音频数据回调
    // 参数：cb - 回调函数，每次采集到音频帧时调用
    void onAudio(AudioCallback cb);

private:
    // captureLoop - 采集循环（在独立线程中运行）
    // 持续从 ALSA 设备读取音频数据，通过回调传递给消费者
    void captureLoop();

    AlsaConfig config_;              // ALSA 采集配置参数
    _snd_pcm* pcm_ = nullptr;        // ALSA PCM 设备句柄
                                     // 指向 ALSA 内部的 PCM 流结构体，
                                     // 由 snd_pcm_open() 创建，snd_pcm_close() 释放

    std::atomic<bool> capturing_{false};  // 采集状态标志（原子变量）
                                     // atomic 保证主线程和采集线程之间的可见性，
                                     // 避免数据竞争导致的未定义行为

    std::thread captureThread_;      // 采集线程
                                     // 在 startCapture() 中创建，stopCapture() 中回收

    AudioCallback audioCb_;          // 音频数据回调函数
                                     // 采集线程中读取到数据后调用此回调
};

} // namespace crystal
