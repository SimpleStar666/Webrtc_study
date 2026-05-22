// ===================================================================================
// sdl_audio_player.h - SDL 音频播放头文件
// ===================================================================================
// 本文件定义了 CrystalRTC 项目中的 SDL 音频播放组件。
//
// 【在 WebRTC 系统中的角色】
// 在 WebRTC 音频接收流水线中，播放模块位于最后端：
// "接收 → Jitter Buffer → 解码 → 播放"
// 解码后的 PCM 音频数据通过 SDL 音频子系统输出到扬声器。
//
// 【SDL 音频子系统核心概念】
// SDL（Simple DirectMedia Layer）提供跨平台的音频输出抽象：
//   1. 音频设备：SDL 管理一个音频输出设备，应用层无需关心底层驱动
//   2. 回调机制：SDL 使用"拉模式"（Pull Model）获取音频数据：
//      - SDL 内部维护一个音频线程，按固定频率调用回调函数
//      - 回调函数负责向 SDL 提供音频数据（填充缓冲区）
//      - 如果回调函数提供数据不够快，会出现"underrun"，导致音频断续
//   3. 音频规格（AudioSpec）：定义采样率、格式、声道数、缓冲区大小等参数
//
// 【SDL 音频队列缓冲机制】
// 本实现使用生产者-消费者模式管理音频数据：
//   - 生产者：解码线程调用 play() 向队列写入 PCM 数据
//   - 消费者：SDL 音频线程通过回调函数从队列读取数据
//   - 缓冲区：使用 std::queue<int16_t> 作为 FIFO 队列
//   - 同步：使用 std::mutex 保护队列，防止生产者和消费者竞争
//
// 这种设计的优缺点：
//   优点：简单直观，线程安全
//   缺点：queue<int16_t> 逐采样点入队出队，性能不如环形缓冲区（Ring Buffer）
//         在高性能场景下应考虑使用无锁环形缓冲区替代
//
// 【音频帧大小与延迟关系】
// SDL 的 samples 参数（AudioSpec.samples）定义了每次回调请求的数据量：
//   samples = 960 表示每次回调请求 960 个采样点（每声道）
//   @48kHz 单声道：960 / 48000 = 20ms 的音频数据
//   这决定了音频输出的最小延迟：SDL 需要至少 20ms 的缓冲数据才能开始播放
// ===================================================================================

#pragma once

#include <cstdint>
#include <vector>
#include <mutex>
#include <queue>

namespace crystal {

// SDLAudioPlayer - SDL 音频播放类
// 职责：将 PCM 音频数据通过 SDL 音频子系统输出到扬声器。
// 设计思路：
//   - 使用 SDL 的回调模式（Pull Model）驱动音频播放
//   - 通过互斥锁保护的队列缓冲区连接解码线程和 SDL 音频线程
//   - 当队列数据不足时，用静音（0）填充，避免音频断续
class SDLAudioPlayer {
public:
    // 构造函数 - 指定音频参数创建播放器对象
    // 参数：
    //   sampleRate - 采样率（Hz），默认 48000，应与解码器输出一致
    //   channels   - 声道数，默认 1（单声道），应与解码器输出一致
    SDLAudioPlayer(int sampleRate = 48000, int channels = 1);

    // 析构函数 - 停止播放并关闭 SDL 音频设备
    ~SDLAudioPlayer();

    // init - 初始化 SDL 音频设备
    // 配置音频规格并打开 SDL 音频设备。
    // 必须在调用 play() 之前调用。
    // 返回值：true 初始化成功，false 初始化失败
    bool init();

    // play - 将 PCM 音频数据送入播放队列
    // 数据会被追加到内部缓冲队列，等待 SDL 回调函数消费。
    // 参数：
    //   data - PCM 采样数据指针，格式为 int16_t，交错排列
    //   samples - 采样点数（每声道），总数据量 = samples × channels
    void play(const int16_t* data, size_t samples);

    // stop - 停止播放并关闭 SDL 音频设备
    void stop();

private:
    // audioCallback - SDL 音频回调函数（静态）
    // SDL 音频线程在需要更多数据时调用此函数。
    // 这是 SDL 的标准回调签名，必须为静态函数或自由函数。
    // 参数：
    //   userdata - 用户数据指针，指向 SDLAudioPlayer 对象实例
    //   stream   - 输出缓冲区，回调函数需向此缓冲区写入音频数据
    //   len      - 请求的数据长度（字节数），不是采样点数
    static void audioCallback(void* userdata, uint8_t* stream, int len);

    // fillBuffer - 填充 SDL 请求的音频数据
    // 从内部缓冲队列中取出数据填充到 SDL 的输出缓冲区。
    // 如果队列中数据不足，用静音（0）填充剩余部分。
    // 参数：
    //   stream - 输出缓冲区指针
    //   len    - 请求的数据长度（字节数）
    void fillBuffer(uint8_t* stream, int len);

    int sampleRate_;                // 采样率（Hz）
    int channels_;                  // 声道数

    std::mutex mutex_;              // 互斥锁，保护 buffer_ 的并发访问
                                    // 生产者线程（play）和消费者线程（audioCallback）
                                    // 通过此互斥锁同步对缓冲队列的访问

    std::queue<int16_t> buffer_;    // 音频数据缓冲队列（FIFO）
                                    // 存储待播放的 PCM 采样数据
                                    // play() 向队尾写入，fillBuffer() 从队头读取
                                    // 注意：此实现逐采样点存储，内存开销较大，
                                    // 生产环境建议使用环形缓冲区（Ring Buffer）优化
};

} // namespace crystal
