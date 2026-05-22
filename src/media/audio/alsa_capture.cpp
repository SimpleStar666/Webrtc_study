// ===================================================================================
// alsa_capture.cpp - ALSA 音频采集实现
// ===================================================================================
// 本文件实现了 ALSA 音频采集的设备打开、参数配置和数据读取逻辑。
//
// 【ALSA PCM 参数配置流程】
// ALSA 的硬件参数配置采用"构建-提交"模式：
//   1. snd_pcm_hw_params_alloca()：在栈上分配参数结构体
//   2. snd_pcm_hw_params_any()：用设备支持的参数范围初始化
//   3. snd_pcm_hw_params_set_xxx()：逐项设置所需的参数
//   4. snd_pcm_hw_params()：将所有参数一次性提交给设备
// 这种模式确保参数的原子性设置，避免设备处于不一致状态。
//
// 【ALSA 错误恢复机制】
// ALSA 采集过程中可能遇到以下错误：
//   - EPIPE（underrun/overrun）：缓冲区欠载/溢出，硬件缓冲区数据不足或过多
//   - ESTRPIPE：声卡被挂起（如电源管理）
// snd_pcm_recover() 可自动恢复这些错误状态，恢复后需重新开始读取。
// ===================================================================================

#include "media/audio/alsa_capture.h"
#include "utils/logger.h"
#include <alsa/asoundlib.h>

namespace crystal {

// 构造函数 - 仅保存配置
AlsaCapture::AlsaCapture(const AlsaConfig& config) : config_(config) {}

// 析构函数 - 自动关闭设备和停止采集
// 确保资源不会泄漏，即使调用者忘记显式关闭
AlsaCapture::~AlsaCapture() {
    close();
}

// open - 打开 ALSA 采集设备并配置硬件参数
// 这是音频采集的第一步，必须成功后才能调用 startCapture()
bool AlsaCapture::open() {
    // 打开 PCM 采集设备
    // 参数说明：
    //   - &pcm_：输出 PCM 句柄
    //   - config_.device.c_str()：设备名称，如 "default"、"hw:0,0"
    //   - SND_PCM_STREAM_CAPTURE：指定为采集流（录音），而非回放流（播放）
    //   - 0：阻塞模式（0=阻塞，SND_PCM_NONBLOCK=非阻塞）
    //     阻塞模式下 snd_pcm_readi 会等待直到有足够数据可读
    int err = snd_pcm_open(&pcm_, config_.device.c_str(),
                           SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        Logger::error("ALSA open failed: {}", snd_strerror(err));
        return false;
    }

    // 以下开始配置 ALSA 硬件参数

    // 在栈上分配硬件参数结构体
    // snd_pcm_hw_params_alloca 使用 alloca 在栈上分配，函数返回时自动释放
    // 比 malloc 更高效，但不适合分配大块内存
    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);

    // 用 PCM 设备支持的参数范围初始化 params
    // 必须在设置任何参数之前调用，将 params 重置为设备的默认能力集
    snd_pcm_hw_params_any(pcm_, params);

    // 设置访问模式为 RW 交错模式
    // SND_PCM_ACCESS_RW_INTERLEAVED：
    //   - RW：使用 readi/writei 函数读写（而非 mmap）
    //   - INTERLEAVED：多声道数据交错排列（L R L R L R...）
    //   对比：SND_PCM_ACCESS_RW_NONINTERLEAVED 非交错排列（LLLL...RRRR...）
    //   交错模式是音频处理中最常用的数据排列方式
    snd_pcm_hw_params_set_access(pcm_, params, SND_PCM_ACCESS_RW_INTERLEAVED);

    // 设置采样格式为 16 位有符号小端序
    // SND_PCM_FORMAT_S16_LE：
    //   - S：Signed（有符号）
    //   - 16：每个采样 16 位（2 字节），动态范围约 96dB
    //   - LE：Little Endian（小端序），x86/ARM 架构的字节序
    // 这是 Opus 编码器要求的输入格式，无需额外格式转换
    snd_pcm_hw_params_set_format(pcm_, params, SND_PCM_FORMAT_S16_LE);

    // 设置声道数
    // 1 = 单声道，语音通话的标准配置
    snd_pcm_hw_params_set_channels(pcm_, params, config_.channels);

    // 设置采样率
    // snd_pcm_hw_params_set_rate_near：设置最接近目标值的采样率
    // 某些声卡不支持精确的 48000Hz，此函数会返回实际设置的采样率。
    // 例如：声卡可能只支持 44100Hz，此时 rate 会被修改为 44100。
    // 参数 &rate：输入目标值，输出实际值
    // 参数 nullptr：方向参数（0=精确匹配，-1=只能小于，1=只能大于）
    unsigned int rate = config_.sampleRate;
    snd_pcm_hw_params_set_rate_near(pcm_, params, &rate, nullptr);

    // 设置周期大小（Period Size）
    // Period 是 ALSA 中硬件中断的基本单位：
    //   - 硬件每处理完一个 period 的数据就触发一次中断
    //   - period_size = 960 帧 = 48kHz × 20ms = 20ms 的音频数据
    //   - 这与 Opus 的帧大小对齐，确保每次读取恰好是一帧编码器的输入
    //
    // Period Size 与延迟的关系：
    //   最小延迟 ≈ period_size / sample_rate
    //   960 / 48000 = 0.020s = 20ms
    //
    // snd_pcm_hw_params_set_period_size_near：
    //   设置最接近目标值的 period 大小，某些声卡要求 period 是特定值的整数倍
    snd_pcm_uframes_t frames = config_.frameSize;
    snd_pcm_hw_params_set_period_size_near(pcm_, params, &frames, nullptr);

    // 将所有硬件参数一次性提交给 PCM 设备
    // ALSA 会检查参数之间的兼容性，如：
    //   - buffer_size 必须是 period_size 的整数倍
    //   - 采样率必须在设备支持的范围内
    // 如果参数不兼容，此函数返回错误
    err = snd_pcm_hw_params(pcm_, params);
    if (err < 0) {
        Logger::error("ALSA hw_params failed: {}", snd_strerror(err));
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return false;
    }

    Logger::info("ALSA capture opened: {}Hz {}ch on {}",
                 config_.sampleRate, config_.channels, config_.device);
    return true;
}

// close - 关闭 ALSA 采集设备
// 先停止采集线程（防止线程访问已关闭的设备），再关闭 PCM 设备
void AlsaCapture::close() {
    stopCapture();
    if (pcm_) {
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
}

// startCapture - 启动音频采集
// 创建独立线程执行采集循环，实现非阻塞采集
void AlsaCapture::startCapture() {
    if (!pcm_) return;

    // 设置采集标志为 true，采集循环中会检查此标志
    capturing_ = true;

    // 创建采集线程，执行 captureLoop 成员函数
    // 使用 std::thread 而非 std::async，因为采集是长期运行的任务
    captureThread_ = std::thread(&AlsaCapture::captureLoop, this);
    Logger::info("ALSA capture started");
}

// stopCapture - 停止音频采集
// 设置停止标志并等待采集线程退出
void AlsaCapture::stopCapture() {
    // 设置 capturing_ 为 false，采集循环会在下次迭代时退出
    capturing_ = false;

    // 等待采集线程结束
    // join() 会阻塞直到线程函数返回，确保线程安全退出
    // 必须在析构前调用，否则 std::thread 析构时会调用 std::terminate
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
}

// captureLoop - 采集循环（在独立线程中运行）
// 持续从 ALSA 设备读取音频数据，通过回调传递给消费者
// 这是音频采集的核心循环，每次迭代读取一帧（20ms）音频数据
void AlsaCapture::captureLoop() {
    // 预分配读取缓冲区
    // 大小 = 帧大小 × 声道数，例如 960 × 1 = 960 个 int16_t
    // 对于交错模式，每个采样点包含所有声道的数据
    std::vector<int16_t> buffer(config_.frameSize * config_.channels);

    while (capturing_) {
        // 从 PCM 设备读取音频数据
        // snd_pcm_readi：
        //   - "i" 后缀表示 interleaved（交错）模式
        //   - 返回实际读取的帧数（每声道），负值表示错误
        //   - 阻塞模式下，会等待直到有 frameSize 帧数据可读
        //   - 每帧包含所有声道的数据（交错排列）
        int frames = snd_pcm_readi(pcm_, buffer.data(), config_.frameSize);

        if (frames < 0) {
            // 读取错误，尝试恢复
            // snd_pcm_recover 可以处理以下错误：
            //   -EPIPE（overrun）：硬件缓冲区溢出，应用程序读取不够快
            //   -ESTRPIPE：声卡被挂起
            //   -EINTR：被信号中断
            // 参数 0：不等待静音（0=立即恢复，1=等待静音后恢复）
            frames = snd_pcm_recover(pcm_, frames, 0);
            if (frames < 0) {
                // 恢复失败，退出采集循环
                Logger::warn("ALSA read failed: {}", snd_strerror(frames));
                break;
            }
            // 恢复成功，跳过本次读取，继续下一轮循环
            continue;
        }

        // 读取成功，通过回调通知消费者
        // 传递实际读取的帧数（可能小于请求的 frameSize）
        if (audioCb_) {
            audioCb_(buffer.data(), static_cast<size_t>(frames));
        }
    }
}

// onAudio - 注册音频数据回调
// 回调函数在采集线程中执行，因此回调函数内部需要注意线程安全：
//   - 不应执行耗时操作，否则会导致 ALSA 缓冲区 overrun
//   - 如需将数据传递到其他线程，应使用线程安全的队列
void AlsaCapture::onAudio(AudioCallback cb) {
    audioCb_ = std::move(cb);
}

} // namespace crystal
