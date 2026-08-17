// ============================================================================
// v4l2_capture.h - V4L2 视频采集设备头文件
// ============================================================================
//
// 【在 WebRTC 系统中的角色】
// V4L2 采集是 WebRTC 视频发送链路的最前端：
//   【V4L2采集】→ YUV原始帧 → H.264编码器 → NAL单元 → RTP打包 → 网络传输
//
// 【核心概念 - V4L2 (Video for Linux 2)】
// V4L2 是 Linux 内核提供的统一视频设备接口，所有 Linux 上的摄像头、采集卡等
// 视频设备都通过 V4L2 API 进行访问和控制。V4L2 定义了一套标准的 ioctl 命令，
// 使应用程序可以不依赖硬件厂商的私有 SDK 就能操作视频设备。
//
// 【V4L2 核心操作流程】
// 1. open()     → 打开设备文件（如 /dev/video0）
// 2. VIDIOC_S_FMT → 设置视频格式（分辨率、像素格式）
// 3. VIDIOC_REQBUFS → 申请缓冲区（mmap 模式）
// 4. VIDIOC_STREAMON → 启动采集流
// 5. VIDIOC_DQBUF / VIDIOC_QBUF → 循环：出队取帧 / 入队还缓冲区
// 6. VIDIOC_STREAMOFF → 停止采集流
// 7. close()    → 关闭设备
//
// 【V4L2 缓冲区模式】
// V4L2 支持三种缓冲区模式：
// - V4L2_MEMORY_MMAP：内存映射模式，最常用。内核分配缓冲区，应用通过 mmap 映射到
//   用户空间访问。零拷贝，性能最优。
// - V4L2_MEMORY_USERPTR：用户空间分配缓冲区，内核直接写入。灵活但需要处理页对齐。
// - V4L2_MEMORY_OVERLAY：覆盖模式，直接写入显存。已过时，很少使用。
//
// 本实现使用 MMAP 模式，这是 V4L2 采集的标准做法。
//
// ============================================================================

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <thread>
#include <atomic>

namespace crystal {

// ============================================================================
// V4L2Config - V4L2 采集设备配置结构体
// ============================================================================
// 封装了打开和配置 V4L2 设备所需的基本参数
struct V4L2Config {
    std::string device = "/dev/video0"; // 设备文件路径
    // Linux 中视频设备通常为 /dev/video0, /dev/video1 等
    // 可通过 ls /dev/video* 或 v4l2-ctl --list-devices 查看

    int width = 640;   // 采集分辨率宽度（像素）
    int height = 480;  // 采集分辨率高度（像素）
    int fps = 30;      // 采集帧率（fps），实际帧率受设备能力限制
};

// ============================================================================
// V4L2Capture - V4L2 视频采集类
// ============================================================================
//
// 【职责】
// 从 Linux V4L2 视频设备采集 YUV 原始帧数据，供编码器使用。
//
// 【设计思路】
// 1. 采用独立线程进行采集（captureLoop），避免阻塞主线程
// 2. 使用回调模式：通过 onFrame() 注册回调，每采集一帧触发一次
// 3. 使用 O_NONBLOCK 非阻塞模式打开设备，配合 DQBUF 的 EAGAIN 处理
// 4. 使用 atomic 标志控制采集线程的启停，保证线程安全
//
// 【采集线程模型】
// 主线程                          采集线程
//   |                               |
//   +-- startCapture() -----------> |
//   |                               +-- while(capturing_) {
//   |                               |     DQBUF (出队取帧)
//   |                               |     frameCb_(yuvData)  ← 回调
//   |                               |     QBUF  (入队还缓冲区)
//   |                               |   }
//   +-- stopCapture() ------------> |
//   |     (join 等待线程退出)        |
//
class V4L2Capture {
public:
    // 帧回调类型
    // 每采集一帧 YUV 数据时触发
    // @param yuvData  YUV420P 数据指针，布局为 Y + U + V 平面
    // @param len      数据长度（字节），= width * height * 3 / 2
    using FrameCallback = std::function<void(const uint8_t* yuvData,
                                             size_t len)>;

    // 构造函数
    // @param config  采集设备配置参数
    explicit V4L2Capture(const V4L2Config& config);

    // 析构函数，自动停止采集并关闭设备
    ~V4L2Capture();

    // 打开视频设备并配置格式和缓冲区
    // @return true 打开成功，false 失败（如设备不存在、格式不支持）
    bool open();

    // 关闭视频设备（先停止采集，再关闭文件描述符）
    void close();

    // 启动视频采集（开启采集线程）
    void startCapture();

    // 停止视频采集（等待采集线程退出）
    void stopCapture();

    // 注册帧回调
    // @param cb  回调函数，每采集一帧时被调用
    void onFrame(FrameCallback cb);

private:
    // 采集循环（在独立线程中运行）
    // 循环执行 DQBUF → 回调 → QBUF 操作，直到 capturing_ 为 false
    void captureLoop();

    V4L2Config config_;              // 采集设备配置
    int fd_ = -1;                    // 设备文件描述符，-1 表示未打开
                                     // V4L2 所有操作都通过此 fd 的 ioctl 完成
    std::atomic<bool> capturing_{false}; // 采集状态标志（原子变量，线程安全）
                                     // 用于控制采集线程的退出
    std::thread captureThread_;      // 采集线程，在 startCapture() 中创建
    FrameCallback frameCb_;          // 帧回调函数
    std::vector<uint8_t> buffer_;    // 帧数据缓冲区
                                     // 注意：当前实现中此缓冲区用于存储采集帧数据
                                     // 在 MMAP 模式下，实际数据在内核映射的内存中
                                     // 此缓冲区作为数据拷贝的中转
};

} // namespace crystal
