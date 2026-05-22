#include "media/video/sdl_renderer.h"
#include "utils/logger.h"
#include <SDL2/SDL.h>

namespace crystal {

SDLRenderer::SDLRenderer(int width, int height, const std::string& title)
    : width_(width), height_(height), title_(title) {}

SDLRenderer::~SDLRenderer() {
    if (texture_) SDL_DestroyTexture(texture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

bool SDLRenderer::init() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        Logger::error("SDL init failed: {}", SDL_GetError());
        return false;
    }

    window_ = SDL_CreateWindow(title_.c_str(),
                                SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED,
                                width_, height_,
                                SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window_) {
        Logger::error("SDL window creation failed: {}", SDL_GetError());
        return false;
    }

    renderer_ = SDL_CreateRenderer(window_, -1,
                                    SDL_RENDERER_ACCELERATED |
                                    SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) {
        Logger::error("SDL renderer creation failed: {}", SDL_GetError());
        return false;
    }

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

void SDLRenderer::render(const uint8_t* yuvData, int width, int height) {
    if (!texture_ || !renderer_) return;

    SDL_UpdateYUVTexture(texture_, nullptr,
                          yuvData, width,
                          yuvData + width * height, width / 2,
                          yuvData + width * height * 5 / 4, width / 2);

    SDL_RenderClear(renderer_);
    SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
    SDL_RenderPresent(renderer_);
}

void SDLRenderer::pollEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            quit_ = true;
        }
    }
}

bool SDLRenderer::shouldQuit() const {
    return quit_;
}

} // namespace crystal
