#include "media/video/v4l2_capture.h"
#include "utils/logger.h"
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <errno.h>

namespace crystal {

V4L2Capture::V4L2Capture(const V4L2Config& config) : config_(config) {}

V4L2Capture::~V4L2Capture() {
    close();
}

bool V4L2Capture::open() {
    fd_ = ::open(config_.device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) {
        Logger::error("Failed to open video device {}: {}",
                      config_.device, strerror(errno));
        return false;
    }

    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = config_.width;
    fmt.fmt.pix.height = config_.height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        Logger::warn("YUV420 not supported, trying MJPEG");
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
            Logger::error("Failed to set video format: {}", strerror(errno));
            ::close(fd_);
            fd_ = -1;
            return false;
        }
    }

    struct v4l2_requestbuffers req = {};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

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

void V4L2Capture::close() {
    stopCapture();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void V4L2Capture::startCapture() {
    if (fd_ < 0) return;

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd_, VIDIOC_STREAMON, &type);

    capturing_ = true;
    captureThread_ = std::thread(&V4L2Capture::captureLoop, this);
    Logger::info("V4L2 capture started");
}

void V4L2Capture::stopCapture() {
    capturing_ = false;
    if (captureThread_.joinable()) {
        captureThread_.join();
    }

    if (fd_ >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd_, VIDIOC_STREAMOFF, &type);
    }
}

void V4L2Capture::captureLoop() {
    buffer_.resize(config_.width * config_.height * 3 / 2);

    while (capturing_) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno != EAGAIN) {
                Logger::warn("V4L2 DQBUF error: {}", strerror(errno));
            }
            usleep(1000);
            continue;
        }

        if (frameCb_ && buf.bytesused > 0) {
            size_t expectedSize = config_.width * config_.height * 3 / 2;
            if (buf.bytesused >= static_cast<unsigned int>(expectedSize)) {
                frameCb_(buffer_.data(), expectedSize);
            }
        }

        ioctl(fd_, VIDIOC_QBUF, &buf);
    }
}

void V4L2Capture::onFrame(FrameCallback cb) {
    frameCb_ = std::move(cb);
}

} // namespace crystal
