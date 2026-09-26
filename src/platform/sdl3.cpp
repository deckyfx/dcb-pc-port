// SDL3 backend: window + 2D renderer with a streaming texture, keyboard / first gamepad on
// port 0, and an SDL audio stream. Built only with -DDCB_USE_SDL3=ON (defines DCB_HAS_SDL3).
//
// Environment:
//   DCB_FILTER=linear   bilinear upscaling (default: nearest)
//   DCB_SCALE=fit       fill the window at 4:3 (default: largest integer multiple of 320x240)

#if defined(DCB_HAS_SDL3)

#include "platform.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <vector>

namespace platform {

namespace {

constexpr int kBaseWidth = 320;   ///< one 4:3 "unit" of output
constexpr int kBaseHeight = 240;
constexpr int kDefaultScale = 3;
constexpr int kChannels = 2;
constexpr int kBytesPerFrame = kChannels * static_cast<int>(sizeof(int16_t));
constexpr int kMaxQueuedFrames = kAudioRate / 10;  ///< ~100 ms latency cap
constexpr Sint16 kTriggerThreshold = 16384;        ///< analog trigger -> L2/R2
constexpr Sint16 kStickThreshold = 16384;          ///< left stick -> D-pad

bool env_equals(const char* name, const char* value) {
    const char* v = SDL_getenv(name);
    return v != nullptr && SDL_strcasecmp(v, value) == 0;
}

class Sdl3 final : public Platform {
public:
    ~Sdl3() override {
        if (gamepad_ != nullptr) SDL_CloseGamepad(gamepad_);
        if (audio_ != nullptr) SDL_DestroyAudioStream(audio_);
        if (texture_ != nullptr) SDL_DestroyTexture(texture_);
        if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
        if (window_ != nullptr) SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    /// Initialise SDL, the window/renderer and (optionally) audio and gamepads.
    bool init(const char* title) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            SDL_Log("dcb: SDL_Init(VIDEO) failed: %s", SDL_GetError());
            return false;
        }
        if (!SDL_CreateWindowAndRenderer(title != nullptr ? title : "dcb", kBaseWidth * kDefaultScale,
                                         kBaseHeight * kDefaultScale, SDL_WINDOW_RESIZABLE, &window_,
                                         &renderer_)) {
            SDL_Log("dcb: cannot create window/renderer: %s", SDL_GetError());
            return false;
        }
        SDL_SetWindowMinimumSize(window_, kBaseWidth, kBaseHeight);
        SDL_SetRenderVSync(renderer_, 0);  // the game paces itself at 59.94 Hz

        // One VRAM-sized texture; each frame updates only the displayed sub-rectangle.
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                     kVramWidth, kVramHeight);
        if (texture_ == nullptr) {
            SDL_Log("dcb: cannot create texture: %s", SDL_GetError());
            return false;
        }
        SDL_SetTextureScaleMode(texture_,
                                env_equals("DCB_FILTER", "linear") ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        integer_scale_ = !env_equals("DCB_SCALE", "fit");
        pixels_.resize(static_cast<size_t>(kVramWidth) * kVramHeight);

        init_audio();
        if (SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            open_first_gamepad();
        } else {
            SDL_Log("dcb: gamepad support unavailable: %s", SDL_GetError());
        }

        // Show black immediately instead of whatever the compositor had.
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        SDL_RenderPresent(renderer_);
        return true;
    }

    bool pump_events() override {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                quit_ = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.scancode == SDL_SCANCODE_ESCAPE) {
                    quit_ = true;
                } else if (ev.key.scancode == SDL_SCANCODE_RETURN && (ev.key.mod & SDL_KMOD_ALT) != 0 &&
                           !ev.key.repeat) {
                    const bool fullscreen = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
                    SDL_SetWindowFullscreen(window_, !fullscreen);
                }
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (gamepad_ == nullptr) gamepad_ = SDL_OpenGamepad(ev.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (gamepad_ != nullptr && SDL_GetGamepadID(gamepad_) == ev.gdevice.which) {
                    SDL_CloseGamepad(gamepad_);
                    gamepad_ = nullptr;
                    open_first_gamepad();
                }
                break;
            default:
                break;
            }
        }
        buttons_ = static_cast<uint16_t>(~(read_keyboard() | read_gamepad()));
        return !quit_;
    }

    uint16_t pad_buttons(unsigned port) const override { return port == 0 ? buttons_ : 0xFFFF; }

    void present(const uint16_t* vram, const DisplayArea& area) override {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);

        const int w = std::clamp(area.width, 0, kVramWidth);
        const int h = std::clamp(area.height, 0, kVramHeight);
        if (area.enabled && vram != nullptr && w > 0 && h > 0) {
            DisplayArea clipped = area;
            clipped.width = w;
            clipped.height = h;
            convert_display(vram, clipped, pixels_.data());
            const SDL_Rect rect{0, 0, w, h};
            SDL_UpdateTexture(texture_, &rect, pixels_.data(), w * static_cast<int>(sizeof(uint32_t)));

            const SDL_FRect src{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)};
            const SDL_FRect dst = output_rect();
            SDL_RenderTexture(renderer_, texture_, &src, &dst);
            blank_frames_ = 0;
        } else {
            draw_loading();
        }
        SDL_RenderPresent(renderer_);
    }

    /// While the game keeps its display off (boot, loading between scenes), show an animated
    /// "Loading..." after half a second so a black window doesn't look like a hang. Host-side
    /// only: the game's picture is never touched.
    void draw_loading() {
        if (++blank_frames_ < 30) return;
        static constexpr const char* kText[] = {"Loading", "Loading.", "Loading..", "Loading..."};
        const char* text = kText[(blank_frames_ / 20) % 4];
        int ww = 0, wh = 0;
        SDL_GetRenderOutputSize(renderer_, &ww, &wh);
        const float scale = std::max(1.0f, static_cast<float>(wh) / 240.0f);  // ~8 px font at 240p
        SDL_SetRenderScale(renderer_, scale, scale);
        const float x = static_cast<float>(ww) / scale - 8.0f * 11.0f;       // room for "Loading..."
        const float y = static_cast<float>(wh) / scale - 16.0f;
        SDL_SetRenderDrawColor(renderer_, 150, 150, 150, 255);
        SDL_RenderDebugText(renderer_, x, y, text);
        SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    }

    void queue_audio(const int16_t* stereo, size_t frames) override {
        if (audio_ == nullptr || stereo == nullptr || frames == 0) return;
        // Keep latency bounded: accept only what fits under the cap and drop the rest.
        const int queued = SDL_GetAudioStreamQueued(audio_) / kBytesPerFrame;
        const int room = kMaxQueuedFrames - std::max(queued, 0);
        if (room <= 0) return;
        const int accepted = static_cast<int>(std::min(frames, static_cast<size_t>(room)));
        SDL_PutAudioStreamData(audio_, stereo, accepted * kBytesPerFrame);
    }

private:
    void init_audio() {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            SDL_Log("dcb: audio unavailable: %s", SDL_GetError());
            return;
        }
        const SDL_AudioSpec spec{SDL_AUDIO_S16, kChannels, kAudioRate};
        audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (audio_ == nullptr) {
            SDL_Log("dcb: cannot open audio device: %s", SDL_GetError());
            return;
        }
        SDL_ResumeAudioStreamDevice(audio_);
    }

    void open_first_gamepad() {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (ids == nullptr) return;
        for (int i = 0; i < count && gamepad_ == nullptr; ++i) gamepad_ = SDL_OpenGamepad(ids[i]);
        SDL_free(ids);
    }

    /// Pressed buttons (active HIGH) from the keyboard.
    static uint16_t read_keyboard() {
        struct Binding {
            SDL_Scancode key;
            uint16_t button;
        };
        static constexpr Binding kBindings[] = {
            {SDL_SCANCODE_UP, Up},         {SDL_SCANCODE_DOWN, Down},       {SDL_SCANCODE_LEFT, Left},
            {SDL_SCANCODE_RIGHT, Right},   {SDL_SCANCODE_Z, Cross},         {SDL_SCANCODE_X, Circle},
            {SDL_SCANCODE_A, Square},      {SDL_SCANCODE_S, Triangle},      {SDL_SCANCODE_RETURN, Start},
            {SDL_SCANCODE_RSHIFT, Select}, {SDL_SCANCODE_BACKSPACE, Select}, {SDL_SCANCODE_Q, L1},
            {SDL_SCANCODE_W, R1},          {SDL_SCANCODE_1, L2},            {SDL_SCANCODE_2, R2},
        };
        const bool* keys = SDL_GetKeyboardState(nullptr);
        uint16_t pressed = 0;
        for (const Binding& b : kBindings)
            if (keys[b.key]) pressed = static_cast<uint16_t>(pressed | b.button);
        // Alt+Enter toggles fullscreen; don't also press Start.
        if ((SDL_GetModState() & SDL_KMOD_ALT) != 0) pressed = static_cast<uint16_t>(pressed & ~Start);
        return pressed;
    }

    /// Pressed buttons (active HIGH) from the open gamepad, standard mapping.
    uint16_t read_gamepad() const {
        if (gamepad_ == nullptr) return 0;
        struct Binding {
            SDL_GamepadButton pad;
            uint16_t button;
        };
        static constexpr Binding kBindings[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH, Cross},
            {SDL_GAMEPAD_BUTTON_EAST, Circle},
            {SDL_GAMEPAD_BUTTON_WEST, Square},
            {SDL_GAMEPAD_BUTTON_NORTH, Triangle},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, Up},
            {SDL_GAMEPAD_BUTTON_DPAD_DOWN, Down},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, Left},
            {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, Right},
            {SDL_GAMEPAD_BUTTON_START, Start},
            {SDL_GAMEPAD_BUTTON_BACK, Select},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, L1},
            {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, R1},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, L3},
            {SDL_GAMEPAD_BUTTON_RIGHT_STICK, R3},
        };
        uint16_t pressed = 0;
        for (const Binding& b : kBindings)
            if (SDL_GetGamepadButton(gamepad_, b.pad)) pressed = static_cast<uint16_t>(pressed | b.button);
        if (SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > kTriggerThreshold) pressed |= L2;
        if (SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > kTriggerThreshold) pressed |= R2;
        // The pad is digital: the left stick doubles as a D-pad.
        const Sint16 lx = SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTX);
        const Sint16 ly = SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTY);
        if (lx < -kStickThreshold) pressed |= Left;
        if (lx > kStickThreshold) pressed |= Right;
        if (ly < -kStickThreshold) pressed |= Up;
        if (ly > kStickThreshold) pressed |= Down;
        return pressed;
    }

    /// Destination rectangle: 4:3, centred, optionally snapped to an integer multiple of 320x240.
    SDL_FRect output_rect() const {
        int ow = 0, oh = 0;
        if (!SDL_GetRenderOutputSize(renderer_, &ow, &oh) || ow <= 0 || oh <= 0) {
            ow = kBaseWidth * kDefaultScale;
            oh = kBaseHeight * kDefaultScale;
        }
        int w = 0, h = 0;
        const int scale = std::min(ow / kBaseWidth, oh / kBaseHeight);
        if (integer_scale_ && scale >= 1) {
            w = kBaseWidth * scale;
            h = kBaseHeight * scale;
        } else if (ow * 3 > oh * 4) {  // window wider than 4:3 -> pillarbox
            h = oh;
            w = oh * 4 / 3;
        } else {                       // taller -> letterbox
            w = ow;
            h = ow * 3 / 4;
        }
        return SDL_FRect{static_cast<float>((ow - w) / 2), static_cast<float>((oh - h) / 2),
                         static_cast<float>(w), static_cast<float>(h)};
    }

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    SDL_AudioStream* audio_ = nullptr;
    SDL_Gamepad* gamepad_ = nullptr;
    std::vector<uint32_t> pixels_;
    uint16_t buttons_ = 0xFFFF;
    int blank_frames_ = 0;  ///< consecutive frames with the display off
    bool integer_scale_ = true;
    bool quit_ = false;
};

}  // namespace

std::unique_ptr<Platform> make_sdl3(const char* title) {
    auto platform = std::make_unique<Sdl3>();
    if (!platform->init(title)) return nullptr;
    return platform;
}

}  // namespace platform

#endif  // DCB_HAS_SDL3
