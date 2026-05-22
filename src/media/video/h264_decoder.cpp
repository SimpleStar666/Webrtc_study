// ============================================================================
// h264_decoder.cpp - H.264 视频解码器实现
// ============================================================================
//
// 【本文件实现要点】
// 1. FFmpeg 解码器的初始化流程（比编码器更简单，无需配置码率等参数）
// 2. AVCC 到 Annex B 格式的转换（添加 00 00 00 01 起始码）
// 3. FFmpeg 异步解码 API（send_packet / receive_frame）的使用
// 4. 解码输出的 YUV 帧数据提取
//
// 【FFmpeg 解码 API 的 send/receive 模型】
// 与编码器对称，解码器也采用异步的 send/receive 模型：
// - avcodec_send_packet()：将压缩数据送入解码器
// - avcodec_receive_frame()：尝试取出一个解码完成的帧
// 一次 send_packet 可能产生 0 个或多个帧（例如收到 SPS/PPS 时不产生帧），
// 也可能需要多个 packet 才能产生一个帧（例如 P 帧需要等待参考帧就绪）。
//
// ============================================================================

#include "media/video/h264_decoder.h"
#include "utils/logger.h"

// extern "C" 包裹 FFmpeg 头文件
// FFmpeg 是 C 语言库，在 C++ 中必须使用 extern "C" 包裹其头文件
// 原因：C++ 编译器会对函数名进行名称修饰（name mangling），而 FFmpeg 的
// 符号是按 C 链接规范导出的。不加 extern "C" 会导致链接时找不到符号。
extern "C" {
#include <libavcodec/avcodec.h>    // 编解码器核心 API
#include <libavutil/imgutils.h>    // 图像工具 API
}
#include <cstring>

namespace crystal {

// ============================================================================
// 构造函数（使用编译器生成的默认实现）
// ============================================================================
// 成员变量已在头文件中初始化（= nullptr / = false），无需额外操作
H264Decoder::H264Decoder() = default;

// ============================================================================
// 析构函数
// ============================================================================
// 按照 FFmpeg 资源的依赖关系逆序释放：
// 1. 先释放 AVPacket（无依赖）
// 2. 再释放 AVFrame（无依赖）
// 3. 最后释放 AVCodecContext（包含解码器全部状态）
// 注意释放顺序：先释放不依赖上下文的对象，再释放上下文本身
H264Decoder::~H264Decoder() {
    if (pkt_) av_packet_free(&pkt_);          // 释放解码输入包
    if (frame_) av_frame_free(&frame_);       // 释放解码输出帧
    if (codecCtx_) avcodec_free_context(&codecCtx_); // 释放解码器上下文
}

// ============================================================================
// H264Decoder::init() - 初始化 H.264 解码器
// ============================================================================
//
// 【FFmpeg 解码器初始化流程】
// 1. avcodec_find_decoder()      - 查找 H.264 解码器
// 2. avcodec_alloc_context3()    - 分配解码器上下文
// 3. 设置解码选项                - 如快速解码标志
// 4. avcodec_open2()             - 打开解码器
// 5. av_frame_alloc()            - 分配解码输出帧
// 6. av_packet_alloc()           - 分配解码输入包
//
// 与编码器不同，解码器不需要设置分辨率、码率等参数，
// 因为这些信息包含在 H.264 码流的 SPS（序列参数集）中，
// 解码器会在解析 SPS 时自动获取并配置。
//
// @return true 初始化成功，false 失败
bool H264Decoder::init() {
    // ---- 第一步：查找 H.264 解码器 ----
    // 使用 AV_CODEC_ID_H264 枚举值查找，FFmpeg 会自动选择最优的 H.264 解码器
    // 可能是软件解码器（如 h264），也可能是硬件加速解码器（如 h264_cuvid、h264_qsv）
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        Logger::error("H264 decoder not found");
        return false;
    }

    // ---- 第二步：分配解码器上下文 ----
    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        Logger::error("Failed to allocate H264 decoder context");
        return false;
    }

    // ---- 第三步：设置快速解码标志 ----
    // AV_CODEC_FLAG2_FAST 允许解码器使用非标准兼容的快速算法
    // 在实时通信中，解码速度比严格的规范兼容性更重要
    // 可能的权衡：某些边缘码流可能出现解码瑕疵，但实际中极少遇到
    codecCtx_->flags2 |= AV_CODEC_FLAG2_FAST;

    // ---- 第四步：打开解码器 ----
    // avcodec_open2 完成解码器的最终初始化
    // 对于解码器，无需传入额外参数（编码器可能需要传入 AVDictionary 类型的选项）
    int ret = avcodec_open2(codecCtx_, codec, nullptr);
    if (ret < 0) {
        Logger::error("Failed to open H264 decoder: {}", ret);
        return false;
    }

    // ---- 第五步：分配帧和包 ----
    // AVFrame：存储解码后的原始视频帧（YUV 数据）
    // AVPacket：存储待解码的压缩数据（H.264 NAL 单元）
    // 解码器持久化这两个对象，避免每次 decode() 都分配/释放
    frame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    initialized_ = true;

    Logger::info("H264 decoder initialized");
    return true;
}

// ============================================================================
// H264Decoder::decode() - 解码一个 NAL 单元
// ============================================================================
//
// 【核心流程】
// 1. 将裸 NAL 数据添加 Annex B 起始码（00 00 00 01）
// 2. 将转换后的数据送入 FFmpeg 解码器
// 3. 循环取出解码完成的帧，触发回调
//
// 【关于 Annex B 起始码】
// FFmpeg 的 H.264 解码器期望输入为 Annex B 格式，即每个 NAL 单元前有
// 起始码 00 00 00 01（或 00 00 01）。而本项目的编码器输出和 RTP 传输
// 使用的是不带起始码的裸 NAL 数据，因此需要在此处添加起始码。
//
// 起始码的作用：
// - 标识 NAL 单元的边界，解码器通过扫描起始码来分割码流
// - 00 00 00 01 是 4 字节起始码，00 00 01 是 3 字节起始码
// - 4 字节起始码用于 SPS/PPS/IDR 等重要 NAL 单元
// - 3 字节起始码用于普通 NAL 单元
// - 本实现统一使用 4 字节起始码，简单可靠
//
// @param nalData  NAL 单元裸数据（不含起始码）
// @param nalLen   NAL 数据长度（字节）
void H264Decoder::decode(const uint8_t* nalData, size_t nalLen) {
    if (!initialized_) return;

    // ---- 构建 Annex B 格式数据 ----
    // 在 NAL 数据前添加 4 字节起始码 00 00 00 01
    // 使用 std::vector 构建连续内存，确保数据在函数作用域内有效
    std::vector<uint8_t> annexB;
    annexB.reserve(4 + nalLen);          // 预分配内存，避免多次 realloc
    annexB.push_back(0x00);              // 起始码第1字节
    annexB.push_back(0x00);              // 起始码第2字节
    annexB.push_back(0x00);              // 起始码第3字节
    annexB.push_back(0x01);              // 起始码第4字节
    annexB.insert(annexB.end(), nalData, nalData + nalLen); // 追加 NAL 数据

    // ---- 将数据设置到 AVPacket ----
    // AVPacket 是 FFmpeg 中存储压缩数据的结构
    // 直接设置 data 和 size，而非使用 av_packet_from_data()
    // 因为我们需要保持对缓冲区生命周期的控制（annexB 在函数结束时释放）
    pkt_->data = annexB.data();
    pkt_->size = static_cast<int>(annexB.size());

    // ---- 将压缩数据送入解码器 ----
    // avcodec_send_packet() 将一个压缩数据包送入解码器的输入队列
    // 返回 0 表示成功，AVERROR(EAGAIN) 表示输入队列已满（需要先 receive_frame）
    // AVERROR_EOF 表示解码器已刷新
    int ret = avcodec_send_packet(codecCtx_, pkt_);
    if (ret < 0) {
        Logger::debug("Failed to send packet to decoder: {}", ret);
        return;
    }

    // ---- 循环取出解码完成的帧 ----
    // avcodec_receive_frame() 尝试从解码器输出队列取出一帧
    // 返回 0 表示成功，AVERROR(EAGAIN) 表示暂无输出
    // 注意：一个 packet 可能不产生帧（如 SPS/PPS），也可能产生一帧
    // 在实时通信中，由于禁用了 B 帧，通常一个 packet 最多产生一帧
    while (avcodec_receive_frame(codecCtx_, frame_) == 0) {
        if (decodedCb_) {
            // 传递解码后的 YUV 数据给回调
            // frame_->data[0] 指向 Y 平面起始地址
            // frame_->width 和 frame_->height 是从 SPS 中解析出的实际分辨率
            // 注意：此指针指向 FFmpeg 内部缓冲区，在下次 avcodec_receive_frame()
            // 或 avcodec_send_packet() 调用后可能失效，上层如需持久化应拷贝数据
            decodedCb_(frame_->data[0], frame_->width, frame_->height);
        }
    }
}

// ============================================================================
// H264Decoder::onDecoded() - 注册解码完成回调
// ============================================================================
// @param cb  回调函数，每帧解码完成时被调用
void H264Decoder::onDecoded(DecodedCallback cb) {
    decodedCb_ = std::move(cb);
}

} // namespace crystal
