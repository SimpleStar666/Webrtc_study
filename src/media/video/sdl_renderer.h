// ============================================================================
// sdl_renderer.h - SDL 视频渲染器头文件
// ============================================================================
//
// 【在 WebRTC 系统中的角色】
// SDL 渲染器是 WebRTC 视频接收链路的最后环节：
//   网络接收 → RTP解包 → H.264解码器 → YUV原始帧 → 【SDL渲染器】→ 屏幕显示
//
// 【核心概念 - SDL (Simple DirectMedia Layer)】
// SDL 是一个跨平台的多媒体开发库，提供了窗口管理、2D 渲染、音频播放、
// 事件处理等功能。在 WebRTC 项目中，SDL 主要用于：
// - 创建渲染窗口
// - 将 YUV 数据渲染到屏幕
// - 处理窗口事件（关闭、调整大小等）
//
// 【核心概念 - YUV 渲染原理】
// 计算机显示器使用 RGB 像素显示图像，而视频编解码使用 YUV 色彩空间。
// SDL 的 SDL_UpdateYUVTexture() API 接受 YUV 输入，由 GPU/渲染器
// 自动完成 YUV→RGB 的色彩空间转换和显示。这利用了 GPU 的硬件加速，
// 比在 CPU 上做 YUV→RGB 转换再渲染要高效得多。
//
// 【SDL 渲染管线】
//   YUV数据 → SDL_UpdateYUVTexture() → SDL_Texture(GPU纹理)
//                                          ↓
//   SDL_RenderClear() → SDL_RenderCopy() → SDL_Renderer(渲染器)
//                                          ↓
//                                  SDL_RenderPresent() → 屏幕
//
// ============================================================================

#pragma once

#include <cstdint>
#include <string>

// SDL 核心结构体的前向声明
// 与 FFmpeg 类似，在头文件中使用前向声明避免包含 SDL 的 C 头文件
// SDL2 本身考虑了 C++ 兼容性（头文件中有 extern "C" 声明），
// 但使用前向声明仍可减少头文件依赖，加快编译速度
struct SDL_Window;    // SDL 窗口，代表屏幕上的一个渲染目标窗口
struct SDL_Renderer;  // SDL 渲染器，负责将纹理绘制到窗口上（可硬件加速）
struct SDL_Texture;   // SDL 纹理，存储像素数据的 GPU 缓冲区

namespace crystal {

// ============================================================================
// SDLRenderer - SDL 视频渲染器类
// ============================================================================
//
// 【职责】
// 创建渲染窗口，将 YUV420P 视频帧渲染到屏幕上。
//
// 【设计思路】
// 1. 封装 SDL 的初始化、窗口创建、渲染器创建、纹理创建等流程
// 2. 提供简洁的 render() 接口，接收 YUV 数据即可渲染
// 3. 提供 pollEvents() / shouldQuit() 接口，支持事件驱动的渲染循环
// 4. 资源管理遵循 RAII 原则，析构时自动释放所有 SDL 资源
//
// 【典型使用模式】
//   SDLRenderer renderer(640, 480);
//   renderer.init();
//   while (!renderer.shouldQuit()) {
//       renderer.pollEvents();
//       renderer.render(yuvData, width, height);
//   }
//
class SDLRenderer {
public:
    // 构造函数
    // @param width   窗口/渲染区域宽度（像素）
    // @param height  窗口/渲染区域高度（像素）
    // @param title   窗口标题
    SDLRenderer(int width, int height, const std::string& title = "CrystalRTC");

    // 析构函数，释放纹理、渲染器、窗口，并调用 SDL_Quit()
    ~SDLRenderer();

    // 初始化 SDL 子系统、创建窗口、渲染器和纹理
    // @return true 初始化成功，false 失败（如 SDL 未安装、GPU 不支持等）
    bool init();

    // 渲染一帧 YUV 数据
    // @param yuvData  YUV420P 平面格式数据指针，布局为 Y + U + V
    // @param width    帧宽度（像素），可能与窗口尺寸不同
    // @param height   帧高度（像素）
    void render(const uint8_t* yuvData, int width, int height);

    // 轮询 SDL 事件（窗口关闭、键盘输入等）
    // 应在渲染循环中频繁调用，否则窗口无法响应系统事件
    void pollEvents();

    // 检查是否应该退出渲染循环
    // @return true 用户关闭了窗口或触发了退出事件
    bool shouldQuit() const;

private:
    int width_;                      // 窗口宽度
    int height_;                     // 窗口高度
    std::string title_;              // 窗口标题
    SDL_Window* window_ = nullptr;   // SDL 窗口对象
    SDL_Renderer* renderer_ = nullptr; // SDL 2D 渲染器，支持硬件加速
                                     // SDL_RENDERER_ACCELERATED 使用 GPU 渲染
                                     // SDL_RENDERER_PRESENTVSYNC 启用垂直同步
    SDL_Texture* texture_ = nullptr; // SDL 纹理对象，存储 YUV 像素数据
                                     // SDL_PIXELFORMAT_IYUV 对应 YUV420P 格式
                                     // SDL_TEXTUREACCESS_STREAMING 允许频繁更新纹理数据
    bool quit_ = false;              // 退出标志，当收到 SDL_QUIT 事件时置为 true
};

} // namespace crystal
