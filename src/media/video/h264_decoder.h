// ============================================================================
// h264_decoder.h - H.264 视频解码器头文件
// ============================================================================
//
// 【在 WebRTC 系统中的角色】
// 在 WebRTC 的视频接收链路中，解码器是"接收端"的核心组件：
//   网络接收 → RTP解包 → NAL单元 → 【H.264解码器】→ YUV原始帧 → 渲染显示
//
// 【核心概念 - H.264 解码】
// 解码是编码的逆过程，将压缩的 H.264 码流还原为可显示的 YUV 视频帧。
// 解码器需要处理的关键问题：
// - 参考帧管理：P 帧依赖前面的 I/P 帧进行解码，解码器需维护参考帧缓冲区
// - 码流格式：接收的 NAL 单元可能是 Annex B 格式或 AVCC 格式，需要正确处理
// - 延迟控制：实时通信中需要尽量降低解码延迟
//
// 【与编码器的设计差异】
// 解码器不需要配置码率、GOP 等参数（这些信息已包含在码流的 SPS/PPS 中），
// 解码器会从码流中自动解析这些参数。因此 H264Decoder 的配置比 H264Encoder 简单。
//
// ============================================================================

#pragma once

#include <vector>
#include <cstdint>
#include <functional>

// FFmpeg 核心结构体的前向声明
// 与编码器头文件相同，使用前向声明避免在头文件中引入 FFmpeg 的 C 声明
// 在 C++ 中包含 FFmpeg 头文件时必须使用 extern "C" 包裹，否则链接失败
struct AVCodecContext;  // 编解码器上下文，保存解码器的全部状态
struct AVFrame;         // 解码后的原始帧数据（YUV 像素）
struct AVPacket;        // 待解码的压缩数据包（H.264 NAL 单元）

namespace crystal {

// ============================================================================
// H264Decoder - H.264 视频解码器类
// ============================================================================
//
// 【职责】
// 将 H.264 NAL 单元数据解码为 YUV420P 原始视频帧，供渲染器显示。
//
// 【设计思路】
// 同样采用回调模式：通过 onDecoded() 注册回调，每当一帧解码完成时，
// 将 YUV 数据传递给上层（如 SDL 渲染器）。
//
// 【解码流程】
// 1. init()：查找并初始化 FFmpeg H.264 解码器
// 2. decode()：接收一个 NAL 单元，转换为 Annex B 格式后送入解码器
// 3. 解码器内部通过 avcodec_send_packet / avcodec_receive_frame 异步处理
// 4. 解码完成后触发回调，传递 YUV 数据
//
// 【关于输入格式】
// 本解码器的 decode() 接口接收裸 NAL 数据（不含起始码，不含长度前缀），
// 内部自动添加 Annex B 起始码（00 00 00 01）后送入 FFmpeg 解码器。
// 这与编码器 processPacket() 的输出格式相匹配，形成完整的编码-解码闭环。
//
class H264Decoder {
public:
    // 解码完成回调类型
    // 当解码器成功解码一帧时，通过此回调通知上层
    // @param yuvData  解码后的 YUV420P 数据指针，布局为 Y + U + V 平面
    //                 注意：此指针指向 FFmpeg 内部缓冲区，在下次 decode() 前有效
    // @param width   解码帧的宽度（像素），从 SPS 中解析得到
    // @param height  解码帧的高度（像素），从 SPS 中解析得到
    using DecodedCallback = std::function<void(const uint8_t* yuvData,
                                                int width, int height)>;

    // 构造函数
    H264Decoder();

    // 析构函数，释放 FFmpeg 解码器资源
    ~H264Decoder();

    // 初始化解码器
    // 查找 H.264 解码器、分配上下文、设置快速解码标志、打开解码器
    // @return true 初始化成功，false 初始化失败
    bool init();

    // 解码一个 NAL 单元
    // 将裸 NAL 数据添加 Annex B 起始码后送入解码器，解码完成后触发 onDecoded 回调
    // @param nalData  NAL 单元裸数据指针（不含起始码，不含长度前缀）
    // @param nalLen   NAL 单元数据长度（字节）
    void decode(const uint8_t* nalData, size_t nalLen);

    // 注册解码完成回调
    // @param cb  回调函数，每帧解码完成时被调用
    void onDecoded(DecodedCallback cb);

private:
    AVCodecContext* codecCtx_ = nullptr; // FFmpeg 解码器上下文，保存解码器全部运行状态
    AVFrame* frame_ = nullptr;           // 解码输出帧结构，存储解码后的 YUV 数据
    AVPacket* pkt_ = nullptr;            // 解码输入包结构，存储待解码的压缩数据
                                         // 与编码器不同，解码器持久化 AVPacket 以避免频繁分配
    bool initialized_ = false;           // 初始化状态标志，防止未初始化时调用 decode()
    DecodedCallback decodedCb_;          // 解码完成回调函数
};

} // namespace crystal
