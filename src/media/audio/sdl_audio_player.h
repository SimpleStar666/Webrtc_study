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
// 【SPSC 无锁环形缓冲（工程化升级 v2 新增）】
// 本实现使用无锁 SPSC 环形缓冲连接解码线程与 SDL 音频线程：
//   - 生产者：解码线程调用 play() 写入 PCM（无锁）
//   - 消费者：SDL 音频线程在回调中读出 PCM（无锁）
//   - 同步：两个原子计数器的 release/acquire 配对（见 spsc_ring.h）
//
// 为什么弃用 mutex+std::queue（v1 实现）：
//   SDL 回调是高优先级实时线程，持锁等解锁 = 优先级反转 + 调度延迟，
//   表现为周期性爆音。实时路径不能有锁是系统编程铁律。
//
// 【水位控制（丢最旧策略）】
//   · push 有界写入：缓冲满时放不下的最新数据被丢弃（爆音一次，不阻塞）
//   · 消费侧回落：水位超过 kCatchUpMs（320ms）时在回调里丢最旧回到
//     kTargetMs（160ms）——时钟漂移/解码过快导致的水位爬升由此收敛，
//     延迟不随时间增长（低延迟优先）。丢最旧必须由消费者执行，
//     原理见 spsc_ring.h 头注释（写方动读指针会破坏无锁正确性）
//
// 【音频帧大小与延迟关系】
// SDL 的 samples 参数（AudioSpec.samples）定义了每次回调请求的数据量：
//   samples = 960 表示每次回调请求 960 个采样点（每声道）
//   @48kHz 单声道：960 / 48000 = 20ms 的音频数据
//   这决定了音频输出的最小延迟：SDL 需要至少 20ms 的缓冲数据才能开始播放
// ===================================================================================

#pragma once

#include "utils/spsc_ring.h"
#include <atomic>
#include <cstdint>

namespace crystal {

// SDLAudioPlayer - SDL 音频播放类
// 职责：将 PCM 音频数据通过 SDL 音频子系统输出到扬声器。
// 设计思路：
//   - 使用 SDL 的回调模式（Pull Model）驱动音频播放
//   - 无锁 SPSC 环形缓冲连接解码线程（生产）和 SDL 音频线程（消费）
//   - 当缓冲数据不足时，用静音（0）填充，避免音频断续，并计数下溢
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

    // play - 将 PCM 音频数据送入播放缓冲（解码线程调用，无锁）
    // 数据会被追加到内部环形缓冲，等待 SDL 回调函数消费。
    // 缓冲满时放不下的最新数据被丢弃并计入溢出统计（见 play() 实现注释）。
    // 参数：
    //   data - PCM 采样数据指针，格式为 int16_t，交错排列
    //   samples - 采样点数（每声道），总数据量 = samples × channels
    void play(const int16_t* data, size_t samples);

    // stop - 停止播放并关闭 SDL 音频设备
    void stop();

    // ---- 观测接口（任意线程，工程化升级 v2 新增）----

    // 当前缓冲水位（毫秒）——喂给指标上报
    size_t bufferedMs() const;

    // 下溢次数累计：回调发现缓冲不足、用静音填过多少次
    // （每回调一次最多计 1，不按采样点计——次数比样本数更好读）
    uint64_t underflowCount() const;

    // 溢出次数累计：缓冲满导致 play() 丢弃最新数据的次数
    uint64_t overflowCount() const;

    // 消费侧丢弃累计：水位回落策略丢弃的最旧采样毫秒数
    uint64_t droppedMs() const;

private:
    // audioCallback - SDL 音频回调函数（静态）
    // SDL 音频线程在需要更多数据时调用此函数。
    // 这是 SDL 的标准回调签名，必须为静态函数或自由函数。
    // 参数：
    //   userdata - 用户数据指针，指向 SDLAudioPlayer 对象实例
    //   stream   - 输出缓冲区，回调函数需向此缓冲区写入音频数据
    //   len      - 请求的数据长度（字节数），不是采样点数
    static void audioCallback(void* userdata, uint8_t* stream, int len);

    // fillBuffer - 填充 SDL 请求的音频数据（SDL 音频线程调用，无锁）
    // 从环形缓冲读出数据填充到 SDL 的输出缓冲区。
    // 不足部分用静音（0）填充并计下溢；水位过高时先丢最旧回落。
    void fillBuffer(uint8_t* stream, int len);

    int sampleRate_;                // 采样率（Hz）
    int channels_;                  // 声道数

    // ---- 无锁环形缓冲（工程化升级 v2：替代 mutex + std::queue）----
    // 容量 = 采样率 × 声道 × 0.5s：足够吸收解码抖动，同时给水位
    // 回落策略（320ms→160ms）留出触发空间
    SpscRing<int16_t> ring_;

    // 下溢计数：仅消费者（SDL 回调线程）写，任意线程读
    // 非原子会有数据竞争；无锁计数本身足够便宜
    std::atomic<uint64_t> underflowCount_{0};

    // ---- 水位控制参数（常量化，见类注释"水位控制"）----
    static constexpr size_t kCatchUpMs = 320;  // 超过此水位触发回落
    static constexpr size_t kTargetMs = 160;   // 回落目标水位
};

} // namespace crystal
