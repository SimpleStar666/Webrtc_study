// ============================================================================
// sdl_renderer.cpp - SDL 视频渲染器实现
// ============================================================================
//
// 【本文件实现要点】
// 1. SDL 子系统初始化（视频 + 音频）
// 2. SDL 窗口、渲染器、纹理的创建流程
// 3. YUV 数据到 SDL 纹理的更新（SDL_UpdateYUVTexture）
// 4. SDL 渲染管线（Clear → Copy → Present）
// 5. SDL 事件处理
//
// 【SDL YUV 渲染原理详解】
// 视频解码后得到的是 YUV420P 数据，而显示器需要 RGB 信号。
// SDL 的渲染管线自动完成 YUV→RGB 转换，其内部流程为：
//
//   1. SDL_UpdateYUVTexture()：
//      将 Y/U/V 三个平面的数据上传到 GPU 纹理
//      GPU 纹理格式为 IYUV（即 YV12/YUV420P），GPU 可直接识别
//
//   2. SDL_RenderCopy()：
//      GPU 执行纹理采样和 YUV→RGB 色彩空间转换
//      转换公式：R = Y + 1.402*(V-128)
//                G = Y - 0.344*(U-128) - 0.714*(V-128)
//                B = Y + 1.772*(U-128)
//      此转换在 GPU 着色器中完成，速度远快于 CPU 实现
//
//   3. SDL_RenderPresent()：
//      将渲染结果（帧缓冲区）提交到显示器
//      启用 VSync 时，此函数会等待下一个垂直同步信号
//
// ============================================================================

#include "media/video/sdl_renderer.h"
#include "utils/logger.h"
#include <SDL2/SDL.h>  // SDL2 主头文件
                        // SDL2 已内置 extern "C" 声明，C++ 中可直接包含
                        // 这与 FFmpeg 不同：FFmpeg 头文件需要手动加 extern "C"

namespace crystal {

// ============================================================================
// 构造函数
// ============================================================================
SDLRenderer::SDLRenderer(int width, int height, const std::string& title)
    : width_(width), height_(height), title_(title) {}

// ============================================================================
// 析构函数
// ============================================================================
// 按照 SDL 资源的依赖关系逆序释放：
// 1. SDL_DestroyTexture（纹理，依赖渲染器）
// 2. SDL_DestroyRenderer（渲染器，依赖窗口）
// 3. SDL_DestroyWindow（窗口）
// 4. SDL_Quit（SDL 子系统，必须在所有 SDL 对象销毁后调用）
SDLRenderer::~SDLRenderer() {
    if (texture_) SDL_DestroyTexture(texture_);    // 销毁纹理，释放 GPU 缓冲区
    if (renderer_) SDL_DestroyRenderer(renderer_);  // 销毁渲染器
    if (window_) SDL_DestroyWindow(window_);        // 销毁窗口
    SDL_Quit();                                     // 关闭 SDL 所有子系统，释放全局资源
}

// ============================================================================
// SDLRenderer::init() - 初始化 SDL 渲染环境
// ============================================================================
//
// 【SDL 初始化流程】
// 1. SDL_Init()          - 初始化 SDL 子系统
// 2. SDL_CreateWindow()  - 创建渲染窗口
// 3. SDL_CreateRenderer() - 创建 2D 渲染器
// 4. SDL_CreateTexture() - 创建 YUV 纹理
//
// @return true 初始化成功，false 失败
bool SDLRenderer::init() {
    // ---- 第一步：初始化 SDL 子系统 ----
    // SDL_INIT_VIDEO：视频子系统，用于窗口管理和渲染
    // SDL_INIT_AUDIO：音频子系统，用于音频播放（WebRTC 音频模块也需要 SDL）
    // SDL_Init 会加载所需的平台驱动（X11/Wayland on Linux, Win32 on Windows 等）
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        Logger::error("SDL init failed: {}", SDL_GetError());
        return false;
    }

    // ---- 第二步：创建窗口 ----
    // SDL_CreateWindow() 创建一个可显示的窗口
    // 参数：
    //   title           窗口标题
    //   SDL_WINDOWPOS_CENTERED  窗口 X 位置居中
    //   SDL_WINDOWPOS_CENTERED  窗口 Y 位置居中
    //   width, height   窗口尺寸
    //   SDL_WINDOW_SHOWN   创建后立即显示
    //   SDL_WINDOW_RESIZABLE  允许用户调整窗口大小
    window_ = SDL_CreateWindow(title_.c_str(),
                                SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED,
                                width_, height_,
                                SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window_) {
        Logger::error("SDL window creation failed: {}", SDL_GetError());
        return false;
    }

    // ---- 第三步：创建渲染器 ----
    // SDL_CreateRenderer() 为窗口创建 2D 渲染器
    // 参数：
    //   window    目标窗口
    //   -1        自动选择第一个可用的渲染驱动
    //   SDL_RENDERER_ACCELERATED  使用硬件加速（GPU 渲染）
    //     - 优先使用 OpenGL/D3D/Vulkan 等图形 API
    //     - 比软件渲染快数十倍
    //   SDL_RENDERER_PRESENTVSYNC  启用垂直同步
    //     - 渲染与显示器刷新率同步，避免画面撕裂（tearing）
    //     - 限制帧率不超过显示器刷新率（通常 60Hz）
    //     - 对 WebRTC 渲染是合适的，因为视频帧率通常 ≤ 30fps
    renderer_ = SDL_CreateRenderer(window_, -1,
                                    SDL_RENDERER_ACCELERATED |
                                    SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) {
        Logger::error("SDL renderer creation failed: {}", SDL_GetError());
        return false;
    }

    // ---- 第四步：创建纹理 ----
    // SDL_CreateTexture() 创建一个用于存储像素数据的 GPU 纹理
    // 参数：
    //   renderer   关联的渲染器
    //   SDL_PIXELFORMAT_IYUV  像素格式为 IYUV（即 YUV420P / YV12）
    //     - IYUV 是 SDL 对 YUV420P 的命名，布局为 Y 平面 + U 平面 + V 平面
    //     - 与 FFmpeg 的 AV_PIX_FMT_YUV420P 完全对应
    //     - GPU 可直接对此格式做 YUV→RGB 转换，无需 CPU 介入
    //   SDL_TEXTUREACCESS_STREAMING  流式访问模式
    //     - 允许频繁更新纹理数据（每帧更新一次）
    //     - 与 STATIC（只设置一次）和 TARGET（作为渲染目标）相对
    //   width_, height_  纹理尺寸，应与视频帧分辨率匹配
    texture_ = SDL_CreateTexture(renderer_,
                                  SDL_PIXELFORMAT_IYUV,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  width_, height_);
    if (!texture_) {
        Logger::error("SDL texture creation failed: {}", SDL_GetError());
        return false;
    }

    Logger::info("SDL renderer initialized: {}x{}", width_, height_);
    return true;
}

// ============================================================================
// SDLRenderer::render() - 渲染一帧 YUV 数据
// ============================================================================
//
// 【SDL 渲染管线（3 步）】
// 1. SDL_UpdateYUVTexture() - 将 YUV 数据上传到 GPU 纹理
// 2. SDL_RenderClear()      - 清除渲染目标（用背景色填充）
// 3. SDL_RenderCopy()       - 将纹理绘制到渲染目标
// 4. SDL_RenderPresent()    - 将渲染结果显示到屏幕
//
// 【关于 YUV 平面布局】
// YUV420P 数据由三个连续的平面组成：
//
//   |<--- width 字节 --->|
//   +--------------------+  ↑
//   |  Y 平面（亮度）     |  | height 行
//   |  每像素 1 字节      |  ↓
//   +----------+         +  ↑
//   | U 平面   |         |  | height/2 行
//   | (Cb色度) |         |  ↓
//   +----------+         +  ↑
//   | V 平面   |         |  | height/2 行
//   | (Cr色度) |         |  ↓
//   +----------+
//
//   Y 平面大小：width × height
//   U 平面大小：(width/2) × (height/2)
//   V 平面大小：(width/2) × (height/2)
//   总大小：width × height × 3/2
//
// @param yuvData  YUV420P 平面格式数据指针
// @param width    帧宽度（像素）
// @param height   帧高度（像素）
void SDLRenderer::render(const uint8_t* yuvData, int width, int height) {
    if (!texture_ || !renderer_) return;

    // SDL_UpdateYUVTexture()：将 YUV 三个平面的数据上传到 GPU 纹理
    // 参数：
    //   texture   目标纹理
    //   nullptr   更新整个纹理（不指定矩形区域）
    //   Y平面指针, Y平面行跨度（pitch/linesize）
    //     - 行跨度 = width，即每行 Y 分量的字节数
    //   U平面指针, U平面行跨度
    //     - U 平面起始 = yuvData + width * height（Y 平面之后）
    //     - 行跨度 = width / 2（水平方向 2:1 子采样）
    //   V平面指针, V平面行跨度
    //     - V 平面起始 = yuvData + width * height * 5/4
    //       （= Y 平面 + U 平面 = width*height + width*height/4）
    //     - 行跨度 = width / 2
    SDL_UpdateYUVTexture(texture_, nullptr,
                          yuvData, width,                              // Y 平面
                          yuvData + width * height, width / 2,        // U 平面
                          yuvData + width * height * 5 / 4, width / 2); // V 平面

    // SDL_RenderClear()：用当前绘制颜色清除渲染目标
    // 在每帧渲染前调用，确保没有上一帧的残留
    SDL_RenderClear(renderer_);

    // SDL_RenderCopy()：将纹理的全部内容绘制到渲染目标的全部区域
    // 两个 nullptr 分别表示源矩形和目标矩形使用整个纹理/整个渲染区域
    // 此操作在 GPU 上执行，包括 YUV→RGB 转换和缩放
    SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);

    // SDL_RenderPresent()：将后台缓冲区的内容提交到前台显示
    // SDL 使用双缓冲机制：渲染在后台缓冲区进行，Present 时交换前后缓冲区
    // 启用 VSync 时，此函数会等待垂直同步信号，避免画面撕裂
    SDL_RenderPresent(renderer_);
}

// ============================================================================
// SDLRenderer::pollEvents() - 轮询 SDL 事件
// ============================================================================
//
// 【SDL 事件系统】
// SDL 通过事件队列通知应用程序各种输入和系统事件：
// - SDL_QUIT：用户点击窗口关闭按钮
// - SDL_KEYDOWN/SDL_KEYUP：键盘输入
// - SDL_WINDOWEVENT：窗口大小变化、焦点变化等
// - SDL_MOUSEMOTION/SDL_MOUSEBUTTONDOWN：鼠标输入
//
// SDL_PollEvent() 从事件队列中取出一个事件，非阻塞。
// 必须在渲染循环中频繁调用，否则：
// 1. 窗口无法响应关闭操作（程序无法退出）
// 2. 窗口无法响应系统事件（被标记为"无响应"）
// 3. 在某些平台上窗口内容不会更新
//
void SDLRenderer::pollEvents() {
    SDL_Event event;
    // 循环取出所有待处理的事件
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            // 用户点击了窗口的关闭按钮，设置退出标志
            quit_ = true;
        }
    }
}

// ============================================================================
// SDLRenderer::shouldQuit() - 检查是否应该退出渲染循环
// ============================================================================
// @return true 用户请求退出
bool SDLRenderer::shouldQuit() const {
    return quit_;
}

} // namespace crystal
