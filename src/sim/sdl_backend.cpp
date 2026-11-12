#include "sim/sdl_backend.hpp"

#include <SDL.h>

#include <algorithm>

namespace rksec::sim {

namespace {

class SdlBackend final : public app::SimBackend {
  public:
    SdlBackend(SDL_Window *w, SDL_Renderer *r, SDL_Texture *t, int width, int height, int scale)
        : window_(w), renderer_(r), texture_(t), width_(width), height_(height), scale_(scale)
    {
    }
    ~SdlBackend() override
    {
        SDL_DestroyTexture(texture_);
        SDL_DestroyRenderer(renderer_);
        SDL_DestroyWindow(window_);
        SDL_Quit();
    }
    SdlBackend(const SdlBackend &) = delete;
    SdlBackend &operator=(const SdlBackend &) = delete;

    const char *name() const override { return "SDL 窗口"; }

    bool pump() override
    {
        SDL_Event e;
        while (SDL_PollEvent(&e) != 0) {
            switch (e.type) {
            case SDL_QUIT:
                return false;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    pressed_ = e.type == SDL_MOUSEBUTTONDOWN;
                    x_ = e.button.x / scale_;
                    y_ = e.button.y / scale_;
                }
                break;
            case SDL_MOUSEMOTION:
                x_ = e.motion.x / scale_;
                y_ = e.motion.y / scale_;
                break;
            default:
                break;
            }
        }
        return true;
    }

    bool present(const std::uint8_t *pixels, std::size_t stride) override
    {
        // SDL 的 ARGB8888 在小端机器上内存顺序是 B G R A，和 XRGB8888 一样，直接拷
        if (SDL_UpdateTexture(texture_, nullptr, pixels, static_cast<int>(stride)) != 0) {
            return false;
        }
        SDL_RenderClear(renderer_);
        SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
        SDL_RenderPresent(renderer_);  // 开了垂直同步：和板子上等翻页一样，帧率不超过屏幕刷新率
        return true;
    }

    void pointer(bool &pressed, int &x, int &y) override
    {
        pressed = pressed_;
        x = std::min(std::max(x_, 0), width_ - 1);
        y = std::min(std::max(y_, 0), height_ - 1);
    }

  private:
    SDL_Window *window_;
    SDL_Renderer *renderer_;
    SDL_Texture *texture_;
    int width_;
    int height_;
    int scale_;
    bool pressed_ = false;
    int x_ = 0;
    int y_ = 0;
};

}  // namespace

std::unique_ptr<app::SimBackend> sdl_backend(int width, int height, int scale, std::string &err)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        err = std::string{"SDL 初始化失败："} + SDL_GetError() +
              "（WSL 需要 Windows 11 的 WSLg；没有图形界面用 --headless）";
        return nullptr;
    }
    SDL_Window *w = SDL_CreateWindow("rk3588-security 界面模拟器", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     width * scale, height * scale, 0);
    if (w == nullptr) {
        err = std::string{"创建窗口失败："} + SDL_GetError();
        SDL_Quit();
        return nullptr;
    }
    SDL_Renderer *r = SDL_CreateRenderer(w, -1, SDL_RENDERER_PRESENTVSYNC);
    if (r == nullptr) {
        r = SDL_CreateRenderer(w, -1, SDL_RENDERER_SOFTWARE);  // 没有硬件加速（虚拟机里常见）
    }
    if (r == nullptr) {
        err = std::string{"创建渲染器失败："} + SDL_GetError();
        SDL_DestroyWindow(w);
        SDL_Quit();
        return nullptr;
    }
    SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (t == nullptr) {
        err = std::string{"创建纹理失败："} + SDL_GetError();
        SDL_DestroyRenderer(r);
        SDL_DestroyWindow(w);
        SDL_Quit();
        return nullptr;
    }
    return std::make_unique<SdlBackend>(w, r, t, width, height, scale);
}

}  // namespace rksec::sim
