// ============================================================================
// h264_encoder.h - H.264 视频编码器头文件
// ============================================================================
//
// 【在 WebRTC 系统中的角色】
// 在 WebRTC 的视频传输链路中，编码器是"发送端"的核心组件：
//   摄像头采集(V4L2) → YUV原始帧 → 【H.264编码器】→ NAL单元 → RTP打包 → 网络传输
//
// 【核心概念 - H.264 编码】
// H.264（也称 AVC，Advanced Video Coding）是目前 WebRTC 中最广泛支持的视频编解码标准。
// 编码的目的是将庞大的原始视频数据（如 YUV420P）压缩为紧凑的码流，以便在有限带宽的
// 网络上实时传输。H.264 通过帧内预测（I帧）、帧间预测（P/B帧）、变换编码（DCT）、
// 熵编码（CABAC/CAVLC）等技术实现高压缩比。
//
// 【关键术语】
// - GOP (Group of Pictures): 图像组，两个I帧之间的帧序列。GOP越大，压缩率越高，
//   但随机访问延迟越大。实时通信中通常设置较小的GOP。
// - Bitrate: 码率，即编码器每秒输出的比特数，直接影响视频质量和带宽占用。
// - Profile: 编码规格，Baseline Profile 适合低延迟实时通信（无B帧、无CABAC），
//   Main/High Profile 压缩率更高但延迟更大。
// - NAL Unit: 网络抽象层单元，H.264码流的基本传输单位，RTP中每个包通常承载一个NAL单元。
//
// 【FFmpeg 编解码框架】
// 本编码器基于 FFmpeg 的 libavcodec 库实现。FFmpeg 提供了统一的编解码 API：
//   avcodec_find_encoder() → avcodec_alloc_context3() → avcodec_open2()
//   → avcodec_send_frame() → avcodec_receive_packet()
// 头文件中使用前向声明（forward declaration）引用 FFmpeg 结构体，避免在头文件中
// 包含 FFmpeg 的 C 头文件，减少编译依赖。
//
// ============================================================================

#pragma once

#include <vector>
#include <cstdint>
#include <functional>

// FFmpeg 核心结构体的前向声明
// 在头文件中使用前向声明而非 #include FFmpeg 头文件，是为了：
// 1. 减少头文件依赖，降低编译时间
// 2. 避免在 C++ 头文件中引入 FFmpeg 的 C 声明
// 注意：FFmpeg 是 C 语言库，在 C++ 代码中包含其头文件时必须使用 extern "C" 包裹，
// 否则会出现链接错误（C++ 的 name mangling 与 C 的符号命名不一致）
struct AVCodecContext;  // 编解码器上下文，保存编解码器的全部状态和参数
struct AVFrame;         // 原始帧数据结构，存储未压缩的音视频数据（如 YUV 像素）
struct AVPacket;        // 压缩数据包结构，存储编码后的码流数据（如 H.264 NAL 单元）

namespace crystal {

// ============================================================================
// H264EncoderConfig - H.264 编码器配置结构体
// ============================================================================
// 封装了编码器初始化所需的关键参数，这些参数直接影响编码质量、延迟和带宽。
// 在 WebRTC 实时通信场景中，参数选择需要在质量、延迟和带宽之间权衡。
struct H264EncoderConfig {
    int width = 640;       // 视频帧宽度（像素），常见值：640(VGA), 1280(720p), 1920(1080p)
    int height = 480;      // 视频帧高度（像素），需与宽度匹配标准分辨率
    int fps = 30;          // 帧率（frames per second），WebRTC 中通常为 15-30fps
    int bitrateKbps = 1000; // 目标码率（千比特/秒），640x480@30fps 通常 500-1500kbps
                           // 码率越高画质越好，但占用带宽越大
    int gopSize = 30;      // GOP 大小（帧数），即每隔多少帧插入一个 I 帧
                           // GOP=30 且 fps=30 时，每秒一个 I 帧
                           // 实时通信中不宜过大，否则影响抗丢包能力和首帧显示速度
};

// ============================================================================
// H264Encoder - H.264 视频编码器类
// ============================================================================
//
// 【职责】
// 将原始 YUV420P 视频帧编码为 H.264 码流（NAL 单元序列），供后续 RTP 打包传输。
//
// 【设计思路】
// 采用回调（Callback）模式：编码器不直接处理网络传输，而是通过 onEncoded() 注册
// 回调函数，每当一个 NAL 单元编码完成时，调用回调将数据传递给上层（如 RTP 打包器）。
// 这种解耦设计使编码器可以独立于传输层进行测试和复用。
//
// 【编码流程】
// 1. init()：查找并初始化 FFmpeg H.264 编码器，配置编码参数
// 2. encode()：接收一帧 YUV 数据，送入编码器
// 3. 编码器内部通过 avcodec_send_frame / avcodec_receive_packet 异步处理
// 4. processPacket()：从编码输出中提取 NAL 单元，触发回调
//
// 【关于 NAL 单元格式】
// FFmpeg libx264 编码器输出 AVCC 格式（4字节长度前缀），而 RTP 传输需要
// Annex B 格式（00 00 00 01 起始码）。processPacket() 负责解析 AVCC 格式，
// 提取每个 NAL 单元的裸数据供上层使用。
//
class H264Encoder {
public:
    // 编码完成回调类型
    // 当编码器产出一个 NAL 单元时，通过此回调通知上层
    // @param nalData  NAL 单元的裸数据指针（不含起始码，不含长度前缀）
    // @param nalLen   NAL 单元数据长度（字节）
    using EncodedCallback = std::function<void(const uint8_t* nalData,
                                                size_t nalLen)>;

    // 构造函数
    // @param config  编码器配置参数，见 H264EncoderConfig 各字段说明
    explicit H264Encoder(const H264EncoderConfig& config);

    // 析构函数，释放 FFmpeg 编码器上下文和帧资源
    ~H264Encoder();

    // 初始化编码器
    // 查找 libx264 编码器、分配上下文、设置编码参数、打开编码器、分配帧缓冲区
    // @return true 初始化成功，false 初始化失败（如编码器未安装）
    bool init();

    // 编码一帧 YUV 数据
    // 将 YUV420P 原始帧送入编码器，编码完成后自动触发 onEncoded 回调
    // @param yuvData  YUV420P 平面格式数据指针，布局为 Y 平面 + U 平面 + V 平面
    //                 总长度 = width * height * 3 / 2
    // @param len      数据长度（字节）
    void encode(const uint8_t* yuvData, size_t len);

    // 注册编码完成回调
    // @param cb  回调函数，每次 NAL 单元编码完成时被调用
    void onEncoded(EncodedCallback cb);

    // 请求下一帧强制编码为 IDR 关键帧（响应 RTCP PLI）
    // 场景：对端解码失败/重传放弃后调用，关键帧可独立解码，使画面立即恢复。
    // 幂等：重复调用在下一帧只产生一个 IDR
    void forceKeyframe();

private:
    // 处理编码输出的 AVPacket
    // 解析 AVCC 格式（4字节大端长度前缀 + NAL 数据），逐个提取 NAL 单元并触发回调
    // @param pkt  FFmpeg 编码输出的压缩数据包
    void processPacket(AVPacket* pkt);

    H264EncoderConfig config_;      // 编码器配置参数
    AVCodecContext* codecCtx_ = nullptr; // FFmpeg 编解码器上下文，保存编码器全部运行状态
    AVFrame* frame_ = nullptr;      // 原始帧结构，用于向编码器送入 YUV 数据
    int64_t pts_ = 0;               // 显示时间戳（Presentation Time Stamp），单调递增
                                    // 编码器按 PTS 顺序输出，确保解码端正确播放顺序
    EncodedCallback encodedCb_;     // 编码完成回调函数
    bool forceKeyframe_ = false;    // 关键帧请求标志（encode 时消费并复位）
};

} // namespace crystal
