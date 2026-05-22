// ============================================================================
// h264_encoder.cpp - H.264 视频编码器实现
// ============================================================================
//
// 【本文件实现要点】
// 1. FFmpeg 编码器的完整初始化流程
// 2. H.264 编码参数的详细配置（码率、GOP、Profile、Preset 等）
// 3. YUV 数据到 FFmpeg AVFrame 的映射
// 4. FFmpeg 异步编码 API（send_frame / receive_packet）的使用
// 5. AVCC 格式 NAL 单元的解析与提取
//
// 【FFmpeg C 头文件在 C++ 中的使用】
// FFmpeg 是纯 C 语言库，其头文件使用 C 链接规范。在 C++ 源文件中包含 FFmpeg 头文件时，
// 必须用 extern "C" { ... } 包裹，否则链接器会因 C++ 的 name mangling 机制
// 找不到 FFmpeg 的符号，导致 undefined reference 链接错误。
//
// ============================================================================

#include "media/video/h264_encoder.h"
#include "utils/logger.h"

// extern "C" 包裹 FFmpeg 头文件 —— 这是 C++ 中使用 C 库的标准做法
// FFmpeg 的所有公开 API 都是用 C 语言定义的，C++ 编译器会对函数名进行
// 名称修饰（name mangling），导致链接时找不到原始符号。extern "C" 告诉
// C++ 编译器按照 C 语言的符号命名规则来处理这些声明。
extern "C" {
#include <libavcodec/avcodec.h>    // 编解码器核心 API：avcodec_find_encoder, avcodec_send_frame 等
#include <libavutil/opt.h>         // 编解码器选项设置 API：av_opt_set，用于设置 preset/tune/profile
#include <libavutil/imgutils.h>    // 图像工具 API：av_frame_get_buffer，用于分配帧缓冲区
}

namespace crystal {

// ============================================================================
// 构造函数
// ============================================================================
// 仅保存配置参数，实际的编码器初始化在 init() 中完成（两阶段初始化模式）
H264Encoder::H264Encoder(const H264EncoderConfig& config)
    : config_(config) {}

// ============================================================================
// 析构函数
// ============================================================================
// 按照 FFmpeg 资源的依赖关系逆序释放：
// 1. 先释放 AVFrame（依赖 AVCodecContext 的像素格式信息）
// 2. 再释放 AVCodecContext（包含编码器全部状态）
// 注意：av_frame_free 和 avcodec_free_context 会将指针置为 nullptr，防止悬空指针
H264Encoder::~H264Encoder() {
    if (frame_) av_frame_free(&frame_);        // 释放原始帧缓冲区
    if (codecCtx_) avcodec_free_context(&codecCtx_); // 释放编码器上下文，同时关闭编码器
}

// ============================================================================
// H264Encoder::init() - 初始化 H.264 编码器
// ============================================================================
//
// 【FFmpeg 编码器初始化流程】
// 1. avcodec_find_encoder_by_name() - 按名称查找编码器（优先使用 libx264）
// 2. avcodec_alloc_context3()       - 分配编码器上下文
// 3. 设置编码参数                   - 码率、分辨率、帧率、GOP、像素格式等
// 4. av_opt_set()                   - 设置编码器私有选项（preset, tune, profile）
// 5. avcodec_open2()                - 打开编码器，完成初始化
// 6. av_frame_alloc() + av_frame_get_buffer() - 分配输出帧缓冲区
//
// @return true 初始化成功，false 失败
bool H264Encoder::init() {
    // ---- 第一步：查找 H.264 编码器 ----
    // 优先尝试 libx264（最成熟的开源 H.264 编码器，质量高、选项丰富）
    // 如果 libx264 不可用（编译时未链接），则回退到系统默认的 H.264 编码器
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        // 回退方案：通过枚举 ID 查找系统中任何可用的 H.264 编码器
        // 可能找到硬件编码器（如 NVIDIA NVENC、Intel QSV 等）
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    }
    if (!codec) {
        Logger::error("H264 encoder not found");
        return false;
    }

    // ---- 第二步：分配编码器上下文 ----
    // AVCodecContext 是 FFmpeg 编解码的核心数据结构，保存了编解码器的全部参数和运行状态
    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        Logger::error("Failed to allocate H264 encoder context");
        return false;
    }

    // ---- 第三步：设置编码参数 ----

    // 码率（bit_rate）：编码器每秒输出的目标比特数
    // config_.bitrateKbps 单位是 kbps，乘以 1000 转换为 bps
    // 码率控制是 WebRTC 自适应带宽（ABR）的关键：当网络带宽变化时，需要动态调整码率
    // FFmpeg 支持多种码率控制模式：CBR（恒定码率）、VBR（可变码率）、ABR（平均码率）
    codecCtx_->bit_rate = config_.bitrateKbps * 1000;

    // 视频分辨率
    codecCtx_->width = config_.width;
    codecCtx_->height = config_.height;

    // 时间基（time_base）：帧时间戳的基本单位，表示每个时间戳刻度代表多少秒
    // {1, fps} 表示每个 PTS 刻度 = 1/fps 秒，即每帧间隔一个 PTS 刻度
    // 例如 fps=30 时，time_base={1,30}，每帧 PTS 递增 1，对应 33.3ms
    // 这是编码端和解码端时间同步的基础，RTP 时间戳也由此推导
    codecCtx_->time_base = {1, config_.fps};

    // 帧率（framerate）：每秒编码的帧数，与 time_base 互为倒数关系
    // framerate = {fps, 1}，编码器据此计算帧间间隔和码率分配
    codecCtx_->framerate = {config_.fps, 1};

    // GOP 大小（gop_size）：两个 I 帧之间的帧数
    // I 帧（关键帧）：完整编码的帧，可独立解码，体积大
    // P 帧：参考前一帧编码的帧，体积中等
    // B 帧：参考前后帧编码的帧，压缩率最高但延迟最大
    // GOP=30 且 fps=30 时，每秒一个 I 帧，平衡了压缩率和随机访问能力
    codecCtx_->gop_size = config_.gopSize;

    // 最大 B 帧数：设为 0 表示不使用 B 帧
    // WebRTC 实时通信中禁用 B 帧的原因：
    // 1. B 帧需要参考后续帧，引入编码延迟（reordering delay）
    // 2. 实时通信对延迟极其敏感，通常要求端到端延迟 < 200ms
    // 3. Baseline Profile 不支持 B 帧
    codecCtx_->max_b_frames = 0;

    // 像素格式：YUV420P（Planar YUV 4:2:0）
    // YUV420P 是视频编码最常用的输入格式：
    // - Y 平面：亮度分量，每个像素 1 字节，分辨率 = width × height
    // - U 平面：蓝色色度分量，2×2 像素共享 1 个采样，分辨率 = width/2 × height/2
    // - V 平面：红色色度分量，同 U 平面
    // 总数据量 = width × height × 1.5 字节（相比 RGB 的 3 字节节省 50%）
    // 4:2:0 子采样利用人眼对色度不敏感的特性，在几乎不降低主观画质的前提下减少数据量
    codecCtx_->pix_fmt = AV_PIX_FMT_YUV420P;

    // 全局头标志：将 SPS/PPS 放入编解码器额外数据而非每个关键帧前
    // AV_CODEC_FLAG_GLOBAL_HEADER 使得 SPS（序列参数集）和 PPS（图像参数集）
    // 被提取到 codecCtx_->extradata 中，而非嵌入每个 IDR 帧的码流中
    // 这在 WebRTC 中很重要：SPS/PPS 通常通过 SDP fmtp 参数或 RTP 的
    // payload descriptor 传递，而非重复包含在每个关键帧中
    codecCtx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // ---- 第四步：设置 libx264 私有选项 ----
    // av_opt_set() 用于设置编码器特有的私有参数，这些参数不在通用 AVCodecContext 中
    // 第一个参数是 codecCtx_->priv_data（编码器私有数据）
    // 最后一个参数 0 表示搜索选项时不使用前缀匹配

    // preset：编码速度与压缩率的权衡
    // 可选值：ultrafast, superfast, veryfast, faster, fast, medium, slow, slower, veryslow
    // ultrafast 牺牲压缩率换取最低编码延迟，是 WebRTC 实时通信的首选
    // 相比 medium preset，ultrafast 的编码速度快 3-5 倍，但码率可能增加 30-50%
    av_opt_set(codecCtx_->priv_data, "preset", "ultrafast", 0);

    // tune：针对特定场景的调优
    // zerolatency 模式禁用所有引入延迟的特性：
    // - 禁用 lookahead（前瞻分析，需要缓冲未来帧）
    // - 禁用 weighted prediction（加权预测）
    // - 禁用 cutree（成本树分析）
    // 这些特性虽然能提高压缩率，但都会引入额外延迟，不适合实时通信
    av_opt_set(codecCtx_->priv_data, "tune", "zerolatency", 0);

    // profile：H.264 编码规格
    // Baseline Profile 特点：
    // - 不支持 B 帧（低延迟）
    // - 不支持 CABAC 熵编码（仅 CAVLC，解码更快）
    // - 不支持隔行扫描
    // - 不支持加权预测
    // Baseline 是 WebRTC 早期强制要求的 Profile，兼容性最好
    // 现代 WebRTC 也支持 Main/High Profile，但 Baseline 仍是安全选择
    av_opt_set(codecCtx_->priv_data, "profile", "baseline", 0);

    // ---- 第五步：打开编码器 ----
    // avcodec_open2 完成编码器的最终初始化，包括验证参数、分配内部缓冲区等
    // 此调用后，编码器参数被锁定，不可再修改
    int ret = avcodec_open2(codecCtx_, codec, nullptr);
    if (ret < 0) {
        Logger::error("Failed to open H264 encoder: {}", ret);
        return false;
    }

    // ---- 第六步：分配帧缓冲区 ----
    // AVFrame 是 FFmpeg 中存储原始（未压缩）音视频数据的结构
    // 对于视频帧，它包含各平面的数据指针和行跨度（linesize）
    frame_ = av_frame_alloc();               // 分配 AVFrame 结构体本身
    frame_->format = AV_PIX_FMT_YUV420P;     // 设置像素格式
    frame_->width = config_.width;           // 设置宽度
    frame_->height = config_.height;         // 设置高度
    av_frame_get_buffer(frame_, 0);          // 根据上述参数分配帧数据缓冲区
                                             // 参数 0 表示使用默认对齐（通常 32 字节，SIMD 优化需要）

    Logger::info("H264 encoder initialized: {}x{} @ {}kbps",
                 config_.width, config_.height, config_.bitrateKbps);
    return true;
}

// ============================================================================
// H264Encoder::encode() - 编码一帧 YUV 数据
// ============================================================================
//
// 【核心流程】
// 1. 验证输入数据长度
// 2. 将 YUV 数据映射到 AVFrame 的各平面
// 3. 设置 PTS（显示时间戳）
// 4. 调用 avcodec_send_frame() 将帧送入编码器输入队列
// 5. 循环调用 avcodec_receive_packet() 取出编码后的数据包
//
// 【FFmpeg 异步编码模型】
// FFmpeg 的编码 API 采用"发送-接收"模式（send/receive model）：
// - avcodec_send_frame()：将原始帧送入编码器，可能立即返回（帧被缓冲）
// - avcodec_receive_packet()：尝试取出一个编码完成的包
// 一次 send 可能产生 0 个或多个 packet，也可能需要多次 send 才产生一个 packet
// 这是因为编码器内部有缓冲和重排逻辑（虽然本配置中禁用了 B 帧，缓冲较小）
//
// @param yuvData  YUV420P 平面格式数据，布局：[Y平面][U平面][V平面]
// @param len      数据总长度（字节）
void H264Encoder::encode(const uint8_t* yuvData, size_t len) {
    // 计算 YUV420P 各平面的预期大小
    // Y 平面：width × height 个像素，每像素 1 字节
    int ySize = config_.width * config_.height;
    // U/V 平面：width/2 × height/2 个像素（4:2:0 子采样，2×2 像素区域共享 1 个色度值）
    int uvSize = ySize / 4;
    // YUV420P 总大小 = Y + U + V = ySize + uvSize + uvSize = ySize * 3 / 2
    int expectedLen = ySize + 2 * uvSize;

    // 数据长度校验：防止缓冲区越界读取
    if (static_cast<int>(len) < expectedLen) {
        Logger::warn("YUV data too short: {} < {}", len, expectedLen);
        return;
    }

    // ---- 将 YUV 数据映射到 AVFrame ----
    // AVFrame 的 data 数组存储各平面的数据指针：
    // data[0] = Y 平面（亮度），data[1] = U 平面（Cb 色度），data[2] = V 平面（Cr 色度）
    // linesize 数组存储各平面每行的字节数（可能大于 width，因为有对齐填充）
    frame_->data[0] = const_cast<uint8_t*>(yuvData);                   // Y 平面起始
    frame_->data[1] = const_cast<uint8_t*>(yuvData + ySize);           // U 平面起始（紧跟 Y 之后）
    frame_->data[2] = const_cast<uint8_t*>(yuvData + ySize + uvSize);  // V 平面起始（紧跟 U 之后）
    frame_->linesize[0] = config_.width;       // Y 平面行跨度 = 视频宽度
    frame_->linesize[1] = config_.width / 2;   // U 平面行跨度 = 视频宽度的一半（水平子采样）
    frame_->linesize[2] = config_.width / 2;   // V 平面行跨度 = 视频宽度的一半

    // 设置显示时间戳（PTS），单调递增
    // PTS 告诉解码器这一帧应该在什么时间显示，是音视频同步的关键
    // 配合 time_base = {1, fps}，PTS 值即为帧序号
    frame_->pts = pts_++;

    // ---- 将帧送入编码器 ----
    // avcodec_send_frame() 将帧放入编码器的输入队列
    // 返回 0 表示成功，负值表示错误（如 AVERROR(EAGAIN) 表示输入队列已满）
    int ret = avcodec_send_frame(codecCtx_, frame_);
    if (ret < 0) {
        Logger::warn("Failed to send frame to encoder: {}", ret);
        return;
    }

    // ---- 循环取出编码后的数据包 ----
    // avcodec_receive_packet() 尝试从编码器输出队列取出一个已编码的包
    // 返回 0 表示成功取出一个包，AVERROR(EAGAIN) 表示暂无输出，AVERROR_EOF 表示编码器已刷新
    // 一次 send_frame 可能产生多个 packet（如 IDR 帧可能同时输出 SPS+PPS+IDR slice）
    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(codecCtx_, pkt) == 0) {
        processPacket(pkt);      // 处理编码输出，提取 NAL 单元
        av_packet_unref(pkt);    // 释放 packet 中引用的缓冲区数据，准备复用
    }
    av_packet_free(&pkt);        // 释放 AVPacket 结构体本身
}

// ============================================================================
// H264Encoder::processPacket() - 处理编码输出的 AVPacket
// ============================================================================
//
// 【AVCC 格式 vs Annex B 格式】
// H.264 码流有两种常见的 NAL 单元封装格式：
//
// 1. AVCC 格式（也叫 AVC1 或 length-prefix 格式）：
//    [4字节大端长度][NAL数据][4字节大端长度][NAL数据]...
//    FFmpeg libx264 编码器默认输出此格式（因为设置了 GLOBAL_HEADER）
//    优点：无需扫描起始码，可直接定位每个 NAL 单元
//
// 2. Annex B 格式：
//    [00 00 00 01][NAL数据][00 00 00 01][NAL数据]...
//    用于 TS 流和本地文件存储，RTP 传输中不使用起始码
//
// 本函数解析 AVCC 格式，提取每个 NAL 单元的裸数据（不含长度前缀），
// 通过回调传递给上层。上层（如 RTP 打包器）会根据 NAL 单元类型
// 决定如何打包：SPS/PPS 通过 SDP 传递，IDR/P 帧数据按 RTP 规则分片。
//
// @param pkt  FFmpeg 编码输出的 AVPacket，其 data 为 AVCC 格式码流
void H264Encoder::processPacket(AVPacket* pkt) {
    const uint8_t* data = pkt->data;  // AVCC 格式码流数据
    int size = pkt->size;             // 码流总长度
    int offset = 0;                   // 当前解析偏移量

    // 循环解析每个 NAL 单元
    while (offset < size) {
        // 读取 4 字节大端序的 NAL 单元长度
        // AVCC 格式中，每个 NAL 单元前有 4 字节表示后续 NAL 数据的长度
        if (offset + 4 > size) break;

        // 大端序读取：第1字节为最高位，第4字节为最低位
        int nalSize = (data[offset] << 24) | (data[offset + 1] << 16) |
                      (data[offset + 2] << 8) | data[offset + 3];
        offset += 4;  // 跳过长度字段

        // 校验 NAL 数据是否完整
        if (offset + nalSize > size) break;

        // 触发回调，传递 NAL 单元裸数据（不含长度前缀，不含起始码）
        // 上层可根据 NAL 第1字节的 type 字段判断 NAL 类型：
        //   type=1 → 非 IDR 切片（P帧数据）
        //   type=5 → IDR 切片（I帧数据）
        //   type=7 → SPS（序列参数集，包含分辨率、Profile 等信息）
        //   type=8 → PPS（图像参数集，包含熵编码模式、参考帧数量等）
        if (encodedCb_) {
            encodedCb_(data + offset, nalSize);
        }
        offset += nalSize;  // 移动到下一个 NAL 单元
    }
}

// ============================================================================
// H264Encoder::onEncoded() - 注册编码完成回调
// ============================================================================
// @param cb  回调函数，每次 NAL 单元编码完成时被调用
void H264Encoder::onEncoded(EncodedCallback cb) {
    encodedCb_ = std::move(cb);
}

} // namespace crystal
