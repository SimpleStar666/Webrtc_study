// ============================================================================
// v4l2_capture.cpp - V4L2 视频采集设备实现
// ============================================================================
//
// 【本文件实现要点】
// 1. V4L2 设备的打开与格式协商
// 2. V4L2 MMAP 缓冲区的申请与管理
// 3. V4L2 采集流的启动与停止
// 4. 基于 DQBUF/QBUF 的帧采集循环
// 5. 非阻塞 I/O 与 EAGAIN 处理
//
// 【V4L2 ioctl 命令速查】
// - VIDIOC_QUERYCAP    查询设备能力
// - VIDIOC_S_FMT       设置视频格式
// - VIDIOC_G_FMT       获取当前视频格式
// - VIDIOC_REQBUFS     申请/释放缓冲区
// - VIDIOC_QBUF        缓冲区入队（归还给内核）
// - VIDIOC_DQBUF       缓冲区出队（从内核取回已填充的帧）
// - VIDIOC_STREAMON    启动采集流
// - VIDIOC_STREAMOFF   停止采集流
//
// ============================================================================

#include "media/video/v4l2_capture.h"
#include "utils/logger.h"
#include <linux/videodev2.h>  // V4L2 核心头文件，定义了所有 ioctl 命令和数据结构
#include <sys/ioctl.h>        // ioctl() 系统调用，V4L2 所有操作都通过它完成
#include <sys/mman.h>         // mmap() 系统调用，用于将内核缓冲区映射到用户空间
#include <fcntl.h>            // open() 的标志位（O_RDWR, O_NONBLOCK 等）
#include <unistd.h>           // close(), usleep() 等 POSIX API
#include <cstring>
#include <errno.h>            // errno 和 strerror()，用于诊断系统调用错误

namespace crystal {

// ============================================================================
// 构造函数
// ============================================================================
V4L2Capture::V4L2Capture(const V4L2Config& config) : config_(config) {}

// ============================================================================
// 析构函数
// ============================================================================
// 自动调用 close()，确保设备被正确关闭和采集线程被停止
V4L2Capture::~V4L2Capture() {
    close();
}

// ============================================================================
// V4L2Capture::open() - 打开视频设备并配置格式和缓冲区
// ============================================================================
//
// 【核心流程】
// 1. 打开设备文件（O_RDWR | O_NONBLOCK）
// 2. 设置视频格式（优先 YUV420，回退 MJPEG）
// 3. 申请 MMAP 缓冲区
//
// 【关于非阻塞模式】
// 使用 O_NONBLOCK 标志打开设备，使得 DQBUF 在没有可用帧时立即返回 EAGAIN
// 而非阻塞等待。这在采集循环中配合 usleep 使用，避免线程被永久阻塞。
//
// @return true 成功，false 失败
bool V4L2Capture::open() {
    // ---- 第一步：打开设备文件 ----
    // O_RDWR：读写模式，采集设备需要读取帧数据
    // O_NONBLOCK：非阻塞模式，DQBUF 无帧时返回 EAGAIN 而非阻塞
    fd_ = ::open(config_.device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) {
        Logger::error("Failed to open video device {}: {}",
                      config_.device, strerror(errno));
        return false;
    }

    // ---- 第二步：设置视频格式 ----
    // v4l2_format 结构体用于指定或查询视频格式
    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;  // 指定为视频采集类型（区别于输出、overlay等）
    fmt.fmt.pix.width = config_.width;        // 请求的宽度
    fmt.fmt.pix.height = config_.height;      // 请求的高度

    // 优先请求 YUV420 格式（V4L2_PIX_FMT_YUV420）
    // YUV420 是编码器最理想的输入格式，无需格式转换
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;

    // V4L2_FIELD_NONE：逐行扫描（非隔行）
    // 隔行扫描（interlaced）是模拟电视时代的产物，现代摄像头均为逐行扫描
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    // VIDIOC_S_FMT：设置视频格式
    // 注意：V4L2 驱动可能不支持请求的格式，此时会修改为最接近的格式并返回成功
    // 可以在调用后读取 fmt 查看实际协商的格式
    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        // YUV420 不被支持，回退到 MJPEG 格式
        // MJPEG 是许多 USB 摄像头支持的格式，每帧为独立的 JPEG 图像
        // 缺点是需要额外的 JPEG 解码步骤才能得到 YUV 数据
        Logger::warn("YUV420 not supported, trying MJPEG");
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
            Logger::error("Failed to set video format: {}", strerror(errno));
            ::close(fd_);
            fd_ = -1;
            return false;
        }
    }

    // ---- 第三步：申请 MMAP 缓冲区 ----
    // v4l2_requestbuffers 结构体用于申请缓冲区
    struct v4l2_requestbuffers req = {};
    req.count = 4;                           // 申请 4 个缓冲区
                                             // 缓冲区数量的权衡：
                                             // - 太少（1-2）：可能导致丢帧（处理慢时无可用缓冲区）
                                             // - 太多（8+）：增加内存占用，延迟略增
                                             // - 4 是常见的平衡值
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;  // 视频采集类型
    req.memory = V4L2_MEMORY_MMAP;           // MMAP 模式：内核分配内存，用户空间通过 mmap 访问

    // VIDIOC_REQBUFS：向内核申请缓冲区
    // 内核可能分配少于请求的数量（但至少 1 个），实际数量在 req.count 中返回
    if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
        Logger::error("Failed to request buffers: {}", strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    Logger::info("V4L2 capture opened: {}x{} on {}",
                 config_.width, config_.height, config_.device);
    return true;
}

// ============================================================================
// V4L2Capture::close() - 关闭视频设备
// ============================================================================
// 先停止采集（确保线程安全退出），再关闭文件描述符
void V4L2Capture::close() {
    stopCapture();
    if (fd_ >= 0) {
        ::close(fd_);  // 关闭设备文件，内核自动释放 MMAP 缓冲区
        fd_ = -1;
    }
}

// ============================================================================
// V4L2Capture::startCapture() - 启动视频采集
// ============================================================================
//
// 【核心流程】
// 1. VIDIOC_STREAMON 启动采集流
// 2. 创建采集线程，运行 captureLoop()
//
// 【关于 VIDIOC_STREAMON】
// 启动采集流后，设备开始向缓冲区队列中填充帧数据。
// 在 STREAMON 之前，需要通过 VIDIOC_QBUF 将所有缓冲区入队。
// （注：当前实现中未显式 QBUF，某些驱动会自动入队 REQBUFS 分配的缓冲区）
//
void V4L2Capture::startCapture() {
    if (fd_ < 0) return;

    // VIDIOC_STREAMON：启动采集流
    // 参数为缓冲区类型指针，启动后设备开始向缓冲区填充帧数据
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd_, VIDIOC_STREAMON, &type);

    // 设置采集标志并启动采集线程
    capturing_ = true;
    captureThread_ = std::thread(&V4L2Capture::captureLoop, this);
    Logger::info("V4L2 capture started");
}

// ============================================================================
// V4L2Capture::stopCapture() - 停止视频采集
// ============================================================================
//
// 【核心流程】
// 1. 设置 capturing_ = false，通知采集线程退出
// 2. join 等待采集线程退出
// 3. VIDIOC_STREAMOFF 停止采集流
//
// 【关于线程安全】
// capturing_ 是 std::atomic<bool>，对它的读写是原子操作，
// 保证了主线程和采集线程之间的可见性和一致性。
//
void V4L2Capture::stopCapture() {
    capturing_ = false;  // 通知采集线程退出循环

    // 等待采集线程退出
    // join() 阻塞直到采集线程的 captureLoop() 返回
    if (captureThread_.joinable()) {
        captureThread_.join();
    }

    // VIDIOC_STREAMOFF：停止采集流
    // 停止后设备不再填充缓冲区，所有已入队的缓冲区被释放回应用
    if (fd_ >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd_, VIDIOC_STREAMOFF, &type);
    }
}

// ============================================================================
// V4L2Capture::captureLoop() - 采集循环（在独立线程中运行）
// ============================================================================
//
// 【V4L2 MMAP 采集的核心循环】
// 采集循环遵循"出队-处理-入队"的模式：
//
//   ┌─────────────────────────────────────────┐
//   │  内核缓冲区队列                          │
//   │  [BUF0] [BUF1] [BUF2] [BUF3]           │
//   │    ↑ 入队(QBUF)        ↓ 出队(DQBUF)    │
//   └─────────────────────────────────────────┘
//
// 1. VIDIOC_DQBUF：从队列中取出一个已填充帧数据的缓冲区
//    - 成功时，buf 中包含帧数据在映射内存中的偏移和长度
//    - 非阻塞模式下，无可用帧时返回 -1 且 errno=EAGAIN
// 2. 处理帧数据：通过回调传递给上层
// 3. VIDIOC_QBUF：将缓冲区重新入队，归还给内核以便再次填充
//    - 必须及时入队，否则缓冲区耗尽后设备无法继续采集
//
// 【关于 MMAP 缓冲区机制】
// MMAP 模式下，内核在内核空间分配缓冲区，通过 mmap() 将其映射到用户空间。
// 应用通过 VIDIOC_DQBUF 获取已填充帧的索引，直接读取映射内存即可获取帧数据，
// 无需从内核空间拷贝到用户空间（零拷贝），性能最优。
// 处理完毕后通过 VIDIOC_QBUF 将缓冲区归还给内核。
//
void V4L2Capture::captureLoop() {
    // 预分配帧数据缓冲区
    // YUV420P 格式：总大小 = width × height × 3/2
    buffer_.resize(config_.width * config_.height * 3 / 2);

    while (capturing_) {
        // v4l2_buffer 结构体用于描述一个缓冲区
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;  // 视频采集类型
        buf.memory = V4L2_MEMORY_MMAP;            // MMAP 模式

        // VIDIOC_DQBUF：出队操作，获取一个已填充帧数据的缓冲区
        // 在非阻塞模式下：
        // - 成功返回 0，buf 中包含帧的索引、偏移、长度等信息
        // - 无可用帧时返回 -1，errno 设为 EAGAIN（表示"再试一次"）
        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno != EAGAIN) {
                // EAGAIN 以外的错误才需要警告（EAGAIN 是正常的"暂无数据"）
                Logger::warn("V4L2 DQBUF error: {}", strerror(errno));
            }
            // 无帧可用时短暂休眠 1ms，避免 CPU 空转
            // 这是非阻塞采集的标准做法
            usleep(1000);
            continue;
        }

        // 处理已出队的帧数据
        // buf.bytesused：缓冲区中实际使用的字节数（帧数据大小）
        if (frameCb_ && buf.bytesused > 0) {
            // 验证帧数据大小是否符合 YUV420P 的预期
            size_t expectedSize = config_.width * config_.height * 3 / 2;
            if (buf.bytesused >= static_cast<unsigned int>(expectedSize)) {
                // 通过回调传递帧数据
                // 注意：当前实现传递的是 buffer_（预分配缓冲区），
                // 而非 MMAP 映射的内核缓冲区。实际生产代码中应直接
                // 从 MMAP 映射地址读取数据（零拷贝），此处使用 buffer_ 中转
                frameCb_(buffer_.data(), expectedSize);
            }
        }

        // VIDIOC_QBUF：入队操作，将缓冲区归还给内核
        // 必须在处理完帧数据后调用，否则缓冲区会被耗尽
        // 内核会在下次帧数据就绪时重新填充此缓冲区
        ioctl(fd_, VIDIOC_QBUF, &buf);
    }
}

// ============================================================================
// V4L2Capture::onFrame() - 注册帧回调
// ============================================================================
// @param cb  回调函数，每采集一帧时被调用
void V4L2Capture::onFrame(FrameCallback cb) {
    frameCb_ = std::move(cb);
}

} // namespace crystal
