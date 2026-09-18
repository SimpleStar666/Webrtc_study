// ===================================================================================
// sdl_audio_player.cpp - SDL 音频播放实现
// ===================================================================================
// 本文件实现了 SDL 音频播放的设备初始化、数据入队和回调填充逻辑。
//
// 【SDL 音频播放工作原理】
// SDL 音频子系统采用"拉模式"（Pull Model）工作：
//   1. 应用层通过 SDL_OpenAudio 打开音频设备，注册回调函数
//   2. SDL 内部创建音频线程，以固定频率调用回调函数
//   3. 回调函数负责向 SDL 提供的缓冲区写入 PCM 数据
//   4. SDL 将缓冲区数据送入声卡驱动，最终输出到扬声器
//
// 【关键注意事项】
// - SDL 回调函数在 SDL 内部音频线程中执行，不是主线程
// - 回调函数必须尽快返回，不能执行阻塞操作（如文件 I/O、网络请求、抢锁）
// - 回调函数中不能调用 SDL_Quit 或 SDL_CloseAudio
// - v2 起回调路径完全无锁（SPSC 环形缓冲），不持有任何锁
// ===================================================================================

#include "media/audio/sdl_audio_player.h"
#include "utils/logger.h"
#include <SDL2/SDL.h>
#include <cstring>

namespace crystal {

// 构造函数 - 保存音频参数并按 0.5s 容量初始化环形缓冲
// （容量换算：采样率 × 声道数 × 0.5 秒；48kHz 单声道 = 24000 个 int16）
SDLAudioPlayer::SDLAudioPlayer(int sampleRate, int channels)
    : sampleRate_(sampleRate), channels_(channels),
      ring_(static_cast<size_t>(sampleRate) * channels / 2) {}

// 析构函数 - 自动停止播放
SDLAudioPlayer::~SDLAudioPlayer() {
    stop();
}

// init - 初始化 SDL 音频设备
// 核心流程：
//   1. 配置 SDL_AudioSpec 结构体
//   2. 打开 SDL 音频设备
//   3. 取消暂停，开始音频输出
// 返回值：true 成功，false 失败
bool SDLAudioPlayer::init() {
    // SDL_AudioSpec - SDL 音频规格结构体
    // 定义了音频设备的所有参数，SDL 会根据此规格打开设备
    SDL_AudioSpec spec;

    // 采样率设置
    // 必须与解码器输出采样率一致，否则会导致播放速度异常
    // 48kHz 是 WebRTC Opus 的标准采样率
    spec.freq = sampleRate_;

    // 采样格式设置
    // AUDIO_S16LSB：16 位有符号小端序
    //   - S16：每个采样 16 位有符号整数，动态范围约 96dB
    //   - LSB：Little Endian（小端序），x86/ARM 架构的标准字节序
    //   - 这与 Opus 解码器输出的 int16_t 格式完全匹配
    //   对比：AUDIO_F32SYS 是 32 位浮点格式，用于需要高精度的场景
    spec.format = AUDIO_S16LSB;

    // 声道数设置
    // 1 = 单声道（语音通话默认）
    // 2 = 立体声
    spec.channels = static_cast<Uint8>(channels_);

    // 缓冲区大小（单位：采样点数 × 声道数）
    // 这个值决定了 SDL 每次回调请求的数据量：
    //   - 960 采样点 @48kHz = 20ms 的音频数据
    //   - 值越大，延迟越高，但抗抖动能力越强
    //   - 值越小，延迟越低，但对数据供给的实时性要求越高
    //   - 960 是 WebRTC 语音通话的标准值
    spec.samples = 960;

    // 回调函数设置
    // SDL 音频线程在需要数据时调用此函数
    // 这是 SDL 的核心机制：应用层不主动"推"数据，而是由 SDL "拉"数据
    spec.callback = audioCallback;

    // 用户数据指针
    // 传递给回调函数的 userdata 参数，通常指向类的实例
    // 回调函数是静态的，通过此指针访问非静态成员
    spec.userdata = this;

    // 打开 SDL 音频设备
    // 参数说明：
    //   - &spec：期望的音频规格
    //   - nullptr：获取的实际规格（此处不关心，传 nullptr）
    //     如果设备不支持请求的规格，SDL 会自动选择最接近的参数
    //     第二个参数可用于获取实际使用的规格
    // 返回值：0 成功，负值失败
    if (SDL_OpenAudio(&spec, nullptr) < 0) {
        Logger::error("SDL audio open failed: {}", SDL_GetError());
        return false;
    }

    // 取消音频设备暂停
    // SDL_OpenAudio 打开设备后，设备默认处于暂停状态（不播放）
    // SDL_PauseAudio(0) 取消暂停，开始音频输出
    // 参数：1=暂停，0=恢复
    // 这给了应用层在开始播放前预填充缓冲区的机会
    SDL_PauseAudio(0);

    Logger::info("SDL audio player initialized: {}Hz {}ch (lock-free SPSC, capacity {}ms)",
                 sampleRate_, channels_, ring_.capacity() / channels_ * 1000 / sampleRate_);
    return true;
}

// play - 将 PCM 音频数据送入播放缓冲（解码线程调用，生产者）
// 无锁写入环形缓冲；缓冲满时放不下的最新数据被丢弃（计入
// pushDropCount，可用 overflowCount() 观测）——宁可爆音一瞬也不
// 阻塞解码线程、不让延迟爬升。
void SDLAudioPlayer::play(const int16_t* data, size_t samples) {
    ring_.push(data, samples * channels_);
}

// stop - 停止播放并关闭 SDL 音频设备
void SDLAudioPlayer::stop() {
    // SDL_CloseAudio 关闭音频设备，停止回调
    // 调用后不会再触发 audioCallback
    SDL_CloseAudio();
}

// audioCallback - SDL 音频回调函数（静态）
// SDL 音频线程在需要数据时调用此函数，这是"拉模式"的核心
// 参数：
//   userdata - 用户数据指针，指向 SDLAudioPlayer 实例
//   stream   - SDL 提供的输出缓冲区，需要向其中写入 PCM 数据
//   len      - 请求的数据长度（字节数！不是采样点数）
//              对于 S16 格式：len / 2 = 采样点数
void SDLAudioPlayer::audioCallback(void* userdata, uint8_t* stream, int len) {
    // 将 userdata 转换回 SDLAudioPlayer 指针
    // 这是 C++ 回调函数的标准模式：通过静态函数 + userdata 访问非静态成员
    auto* self = static_cast<SDLAudioPlayer*>(userdata);
    self->fillBuffer(stream, len);
}

// fillBuffer - 填充 SDL 请求的音频数据（SDL 音频线程调用，消费者）
// 从环形缓冲读出数据填充到 SDL 的输出缓冲区
// 三个职责，全程无锁：
//   1. 水位回落：超 320ms → 丢最旧回到 160ms（延迟不爬升）
//   2. 正常消费：pop 出本次回调需要的采样数
//   3. 下溢兜底：不足部分填静音（0），并计一次下溢
void SDLAudioPlayer::fillBuffer(uint8_t* stream, int len) {
    // 将字节流缓冲区转换为 int16_t 指针
    // SDL 传递的 stream 是 uint8_t* 字节流，但实际数据格式是 int16_t
    // reinterpret_cast 进行类型转换，不改变底层数据
    int16_t* out = reinterpret_cast<int16_t*>(stream);

    // 计算需要填充的采样点数
    // len 是字节数，每个 int16_t 占 2 字节
    // 例如：len=1920 字节 → 960 个采样点（48kHz 单声道 20ms）
    size_t samples = static_cast<size_t>(len) / 2;

    // ---- 1. 水位回落（丢最旧，消费者侧执行才安全）----
    // 缓冲水位超过 kCatchUpMs 说明生产持续快于消费（时钟漂移/解码
    // 过快）。丢最旧直接跳到 kTargetMs 水位：听感上是跳过一小段，
    // 但延迟立刻收敛回来——实时通话低延迟优先于连续性。
    size_t bufferedSamples = ring_.size();
    size_t catchUpSamples = kCatchUpMs * sampleRate_ / 1000 * channels_;
    size_t targetSamples = kTargetMs * sampleRate_ / 1000 * channels_;
    if (bufferedSamples > catchUpSamples) {
        ring_.drop(bufferedSamples - targetSamples);
    }

    // ---- 2. 正常消费：pop 本次回调需要的数据 ----
    size_t got = ring_.pop(out, samples);

    // ---- 3. 下溢兜底：不足部分填静音 ----
    // 静音填充是音频播放的标准做法：
    //   - 避免播放未初始化的内存数据（会产生噪声）
    //   - 保持音频流的连续性，避免设备状态异常
    // 下溢说明数据供给不足（网络延迟/解码跟不上/JitterBuffer 耗尽），
    // 计数供指标上报定位问题
    if (got < samples) {
        std::memset(out + got, 0, (samples - got) * sizeof(int16_t));
        underflowCount_.fetch_add(1, std::memory_order_relaxed);
    }
}

// ---- 观测接口（工程化升级 v2 新增）----

// 当前缓冲水位（毫秒）：样本数 → 毫秒
size_t SDLAudioPlayer::bufferedMs() const {
    return ring_.size() * 1000 / (static_cast<size_t>(sampleRate_) * channels_);
}

// 下溢次数：用静音填过多少次回调
uint64_t SDLAudioPlayer::underflowCount() const {
    return underflowCount_.load(std::memory_order_relaxed);
}

// 溢出次数：缓冲满导致 play() 丢弃最新数据的次数
uint64_t SDLAudioPlayer::overflowCount() const {
    return ring_.pushDropCount();
}

// 消费侧丢弃累计（采样毫秒数）：水位回落策略丢掉的时长
uint64_t SDLAudioPlayer::droppedMs() const {
    return ring_.consumerDropCount() * 1000 /
           (static_cast<size_t>(sampleRate_) * channels_);
}

} // namespace crystal
