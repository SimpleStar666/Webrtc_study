#pragma once

#include <cstdint>
#include <string>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace crystal {

class SDLRenderer {
public:
    SDLRenderer(int width, int height, const std::string& title = "CrystalRTC");
    ~SDLRenderer();

    bool init();
    void render(const uint8_t* yuvData, int width, int height);
    void pollEvents();
    bool shouldQuit() const;

private:
    int width_;
    int height_;
    std::string title_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    bool quit_ = false;
};

} // namespace crystal
