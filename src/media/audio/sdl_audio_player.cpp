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
// - 回调函数必须尽快返回，不能执行阻塞操作（如文件 I/O、网络请求）
// - 回调函数中不能调用 SDL_Quit 或 SDL_CloseAudio
// - 必须使用互斥锁保护回调函数与主线程共享的数据
// ===================================================================================

#include "media/audio/sdl_audio_player.h"
#include "utils/logger.h"
#include <SDL2/SDL.h>
#include <cstring>

namespace crystal {

// 构造函数 - 保存音频参数
SDLAudioPlayer::SDLAudioPlayer(int sampleRate, int channels)
    : sampleRate_(sampleRate), channels_(channels) {}

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
    spec.channels = channels_;

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

    Logger::info("SDL audio player initialized: {}Hz {}ch", sampleRate_, channels_);
    return true;
}

// play - 将 PCM 音频数据送入播放队列
// 此函数由解码线程调用（生产者），将解码后的 PCM 数据追加到缓冲队列
void SDLAudioPlayer::play(const int16_t* data, size_t samples) {
    // 加锁保护队列操作
    // lock_guard 在作用域结束时自动释放锁（RAII）
    std::lock_guard<std::mutex> lock(mutex_);

    // 将所有采样点逐个入队
    // 注意：samples × channels_ 是总采样点数（包含所有声道）
    // 对于交错排列的数据：[L0, R0, L1, R1, ...]，每个元素作为一个 int16_t 入队
    for (size_t i = 0; i < samples * channels_; i++) {
        buffer_.push(data[i]);
    }
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

// fillBuffer - 填充 SDL 请求的音频数据
// 从内部缓冲队列取出数据填充到 SDL 的输出缓冲区
void SDLAudioPlayer::fillBuffer(uint8_t* stream, int len) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 将字节流缓冲区转换为 int16_t 指针
    // SDL 传递的 stream 是 uint8_t* 字节流，但实际数据格式是 int16_t
    // reinterpret_cast 进行类型转换，不改变底层数据
    int16_t* out = reinterpret_cast<int16_t*>(stream);

    // 计算需要填充的采样点数
    // len 是字节数，每个 int16_t 占 2 字节
    // 例如：len=1920 字节 → 960 个采样点（48kHz 单声道 20ms）
    int samples = len / 2;

    for (int i = 0; i < samples; i++) {
        if (!buffer_.empty()) {
            // 从队列头部取出一个采样点
            out[i] = buffer_.front();
            buffer_.pop();
        } else {
            // 队列为空，用静音（0）填充
            // 静音填充是音频播放的标准做法：
            //   - 避免播放未初始化的内存数据（会产生噪声）
            //   - 保持音频流的连续性，避免设备状态异常
            // 队列空说明数据供给不足（underrun），可能原因：
            //   - 网络延迟导致解码数据未及时到达
            //   - 解码速度跟不上播放速度
            //   - Jitter Buffer 中的数据已耗尽
            out[i] = 0;
        }
    }
}

} // namespace crystal
