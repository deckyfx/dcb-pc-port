// SDL3 backend: window + 2D renderer with a streaming texture, keyboard / first gamepad on
// port 0, and an SDL audio stream. Built only with -DDCB_USE_SDL3=ON (defines DCB_HAS_SDL3).
//
// Options come from settings.ini (see settings.hpp for its location and format). Environment
// variables override the file without being written back:
//   DCB_SETTINGS=<path>          use this settings file
//   DCB_FILTER=linear|nearest    upscaling filter
//   DCB_SCALE=fit|integer        fill the window, or whole multiples of 320x240

#if defined(DCB_HAS_SDL3)

#include "platform.hpp"
#include "sdl3_trainer.hpp"
#include "settings.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <system_error>
#include <vector>

namespace platform {

namespace {

constexpr int kBaseWidth = 320;   ///< one 4:3 "unit" of output
constexpr int kBaseHeight = 240;
constexpr int kChannels = 2;
constexpr int kBytesPerFrame = kChannels * static_cast<int>(sizeof(int16_t));
constexpr int kMaxQueuedFrames = kAudioRate / 10;  ///< ~100 ms latency cap

class Sdl3 final : public Platform {
public:
    ~Sdl3() override {
        if (gamepad_ != nullptr) SDL_CloseGamepad(gamepad_);
        if (audio_ != nullptr) SDL_DestroyAudioStream(audio_);
        if (movie_texture_ != nullptr) SDL_DestroyTexture(movie_texture_);
        if (texture_ != nullptr) SDL_DestroyTexture(texture_);
        if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
        if (window_ != nullptr) SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    /// Initialise SDL, the window/renderer and (optionally) audio and gamepads, configured
    /// from the settings file at `settings_path` (created with defaults if missing).
    bool init(const char* title, const std::filesystem::path& settings_path) {
        load_settings(settings_path);
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            SDL_Log("dcb: SDL_Init(VIDEO) failed: %s", SDL_GetError());
            return false;
        }
        const int scale = settings_.display.scale;
        SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
        if (settings_.display.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;  // borderless desktop
        if (!SDL_CreateWindowAndRenderer(title != nullptr ? title : "dcb", kBaseWidth * scale, kBaseHeight * scale,
                                         flags, &window_, &renderer_)) {
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
        SDL_SetTextureScaleMode(texture_, settings_.display.filter == FilterMode::Linear ? SDL_SCALEMODE_LINEAR
                                                                                        : SDL_SCALEMODE_NEAREST);
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
            if (trainer_ != nullptr && trainer_handle_event(*trainer_, window_, ev, settings_.trainer_keys)) {
                key_held_.fill(false);  // the panel owns the keyboard; no stuck pad buttons after it
                continue;
            }
            switch (ev.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                quit_ = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (trace_input_ && !ev.key.repeat)
                    SDL_Log("dcb: key down '%s' (scancode %d)", SDL_GetScancodeName(ev.key.scancode),
                            static_cast<int>(ev.key.scancode));
                if (ev.key.scancode < SDL_SCANCODE_COUNT) {
                    key_held_[ev.key.scancode] = true;
                    key_tap_frames_[ev.key.scancode] = kMinTapFrames;
                }
                if (ev.key.scancode == SDL_SCANCODE_ESCAPE) {
                    quit_ = true;
                } else if (is_enter(ev.key.scancode) && (ev.key.mod & SDL_KMOD_ALT) != 0 && !ev.key.repeat) {
                    toggle_fullscreen();
                } else if (!ev.key.repeat && bound(settings_.overlay_keys, ev.key.scancode)) {
                    overlay_visible_ = !overlay_visible_;
                } else if (!ev.key.repeat && bound(settings_.pause_keys, ev.key.scancode)) {
                    commands_ |= kTogglePause;
                } else if (bound(settings_.frame_advance_keys, ev.key.scancode)) {
                    commands_ |= kFrameAdvance;  // key repeat steps frame by frame while held
                } else if (bound(settings_.fast_forward_keys, ev.key.scancode)) {
                    // held state is read from key_held_ by fast_forward()
                } else if (!ev.key.repeat && bound(settings_.scale_mode_keys, ev.key.scancode)) {
                    toggle_scale_mode();
                } else if (const uint32_t state = state_command(ev.key.scancode)) {
                    if (!ev.key.repeat) commands_ |= state;
                } else if (!ev.key.repeat) {
                    any_press_ = true;
                }
                break;
            case SDL_EVENT_KEY_UP:
                if (trace_input_)
                    SDL_Log("dcb: key up '%s'", SDL_GetScancodeName(ev.key.scancode));
                if (ev.key.scancode < SDL_SCANCODE_COUNT) key_held_[ev.key.scancode] = false;
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                key_held_.fill(false);  // key-ups are not delivered to an unfocused window
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                any_press_ = true;
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
        const uint16_t buttons = static_cast<uint16_t>(~(read_keyboard() | read_gamepad()));
        if (trace_input_ && buttons != buttons_) {
            std::string names;
            for (const PadButtonInfo& b : kPadButtons)
                if ((buttons & b.bit) == 0) names += std::string(names.empty() ? "" : "+") + b.name;
            SDL_Log("dcb: pad %04X %s", buttons, names.empty() ? "(released)" : names.c_str());
        }
        buttons_ = buttons;
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
        } else if (!paused_) {
            draw_loading();
        }
        if (paused_) draw_label("PAUSED");
        else if (fast_forward()) draw_label("FF >>");
        draw_message();
        if (overlay_visible_) draw_overlay();
        if (trainer_ != nullptr && trainer_->is_open()) trainer_draw(renderer_, *trainer_);
        SDL_RenderPresent(renderer_);
    }

    void present_movie(const uint8_t* rgb, int w, int h) override {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        if (rgb != nullptr && w > 0 && h > 0) {
            if (movie_texture_ == nullptr || movie_w_ != w || movie_h_ != h) {
                if (movie_texture_ != nullptr) SDL_DestroyTexture(movie_texture_);
                movie_texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, w, h);
                movie_w_ = w;
                movie_h_ = h;
                if (movie_texture_ != nullptr) SDL_SetTextureScaleMode(movie_texture_, SDL_SCALEMODE_LINEAR);
            }
            if (movie_texture_ != nullptr) {
                SDL_UpdateTexture(movie_texture_, nullptr, rgb, w * 3);
                // The game's 4:3 area, then the movie centred in it at its own aspect ratio (a
                // 320x160 movie letterboxes inside it, as on the console).
                const SDL_FRect area = output_rect();
                SDL_FRect dst = area;
                const float aspect = static_cast<float>(w) / static_cast<float>(h);
                if (area.w / area.h < aspect) {
                    dst.h = area.w / aspect;
                    dst.y = area.y + (area.h - dst.h) / 2.0f;
                } else {
                    dst.w = area.h * aspect;
                    dst.x = area.x + (area.w - dst.w) / 2.0f;
                }
                // Game movies are 320 wide inside a 320x240 screen: keep that width relationship
                // (a 320x160 movie spans the full width and 2/3 of the height).
                SDL_RenderTexture(renderer_, movie_texture_, nullptr, &dst);
            }
        }
        if (paused_) draw_label("PAUSED");
        draw_message();
        if (overlay_visible_) draw_overlay();
        SDL_RenderPresent(renderer_);
    }

    void set_stats(const FrameStats& stats) override { stats_ = stats; }

    uint32_t take_commands() override {
        const uint32_t c = commands_;
        commands_ = 0;
        return c;
    }

    void set_paused(bool paused) override { paused_ = paused; }

    bool fast_forward() const override {
        for (const int code : settings_.fast_forward_keys)
            if (code >= 0 && code < static_cast<int>(SDL_SCANCODE_COUNT) && key_held_[static_cast<size_t>(code)]) return true;
        return false;
    }
    void attach_trainer(trainer::Trainer* trainer) override { trainer_ = trainer; }

    bool take_any_press() override {
        const bool pressed = any_press_;
        any_press_ = false;
        return pressed;
    }

    /// Performance panel in the top-right corner (toggled by [hotkeys] overlay, default F3).
    void draw_overlay() {
        const int queued = audio_ ? SDL_GetAudioStreamQueued(audio_) / kBytesPerFrame : 0;
        char lines[5][40];
        std::snprintf(lines[0], sizeof lines[0], "FPS      %5.1f", stats_.fps);
        std::snprintf(lines[1], sizeof lines[1], "Game FPS %5.1f", stats_.game_fps);
        std::snprintf(lines[2], sizeof lines[2], "CPU      %4.0f%%", stats_.cpu_pct);
        std::snprintf(lines[3], sizeof lines[3], "GPU      %4.0f%%", stats_.gpu_pct);
        std::snprintf(lines[4], sizeof lines[4], "Audio    %3d ms", queued * 1000 / kAudioRate);

        int ww = 0, wh = 0;
        SDL_GetRenderOutputSize(renderer_, &ww, &wh);
        const float scale = std::max(1.0f, static_cast<float>(wh) / 360.0f);  // stays readable, not huge
        SDL_SetRenderScale(renderer_, scale, scale);
        constexpr float kLine = 10.0f, kWidth = 8.0f * 15.0f + 8.0f;
        const float x = static_cast<float>(ww) / scale - kWidth - 4.0f, y = 4.0f;
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 170);
        const SDL_FRect panel{x, y, kWidth, kLine * 5.0f + 6.0f};
        SDL_RenderFillRect(renderer_, &panel);
        SDL_SetRenderDrawColor(renderer_, 120, 255, 120, 255);
        for (int i = 0; i < 5; ++i) SDL_RenderDebugText(renderer_, x + 4.0f, y + 4.0f + kLine * static_cast<float>(i), lines[i]);
        SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    }

    /// While the game keeps its display off (boot, loading between scenes), show an animated
    /// "Loading..." after half a second so a black window doesn't look like a hang. Host-side
    /// only: the game's picture is never touched.
    /// A short status label ("PAUSED", "FF >>") in the top-left corner.
    void draw_label(const char* text) {
        int ww = 0, wh = 0;
        SDL_GetRenderOutputSize(renderer_, &ww, &wh);
        const float scale = std::max(1.0f, static_cast<float>(wh) / 240.0f);
        SDL_SetRenderScale(renderer_, scale, scale);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 170);
        const SDL_FRect panel{4.0f, 4.0f, 8.0f * static_cast<float>(std::strlen(text)) + 8.0f, 16.0f};
        SDL_RenderFillRect(renderer_, &panel);
        SDL_SetRenderDrawColor(renderer_, 255, 220, 120, 255);
        SDL_RenderDebugText(renderer_, 8.0f, 8.0f, text);
        SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    }

    static bool bound(const std::vector<int>& keys, SDL_Scancode sc) {
        return std::find(keys.begin(), keys.end(), static_cast<int>(sc)) != keys.end();
    }

    // ---- Save states: hotkeys and the on-screen notice ------------------------------------------

    /// HostCommand bit for a save-state hotkey, or 0.
    uint32_t state_command(SDL_Scancode sc) const {
        if (bound(settings_.save_state_keys, sc)) return kSaveState;
        if (bound(settings_.load_state_keys, sc)) return kLoadState;
        if (bound(settings_.state_slot_keys, sc)) return kNextStateSlot;
        return 0;
    }

    void show_message(const std::string& text) override {
        message_ = text;
        message_until_ = SDL_GetTicks() + kMessageMs;
    }

    void clear_audio() override {
        if (audio_ != nullptr) SDL_ClearAudioStream(audio_);
    }

    /// The last notice, below the PAUSED label, drawn the same way, until it expires.
    void draw_message() {
        if (message_.empty()) return;
        if (SDL_GetTicks() >= message_until_) {
            message_.clear();
            return;
        }
        int ww = 0, wh = 0;
        SDL_GetRenderOutputSize(renderer_, &ww, &wh);
        const float scale = std::max(1.0f, static_cast<float>(wh) / 240.0f);
        SDL_SetRenderScale(renderer_, scale, scale);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 170);
        const float y = paused_ ? 24.0f : 4.0f;
        const SDL_FRect panel{4.0f, y, 8.0f * static_cast<float>(message_.size()) + 8.0f, 16.0f};
        SDL_RenderFillRect(renderer_, &panel);
        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
        SDL_RenderDebugText(renderer_, 8.0f, y + 4.0f, message_.c_str());
        SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    }

    static constexpr uint64_t kMessageMs = 2000;
    std::string message_;
    uint64_t message_until_ = 0;

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
        if (settings_.volume != kVolumeMax) {
            const size_t samples = static_cast<size_t>(accepted) * kChannels;
            scaled_.resize(samples);
            apply_volume(stereo, scaled_.data(), samples, settings_.volume);
            stereo = scaled_.data();
        }
        SDL_PutAudioStreamData(audio_, stereo, accepted * kBytesPerFrame);
    }

private:
    static bool is_enter(SDL_Scancode sc) { return sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER; }

    /// Load settings.ini (writing defaults on first run), log any warnings, apply env overrides.
    void load_settings(const std::filesystem::path& path) {
        std::vector<std::string> warnings;
        settings_file_ = SettingsFile::load_or_create(path, warnings);
        settings_ = parse_settings(settings_file_.document(), sdl3_binding_resolvers(), warnings);
        apply_env_overrides(settings_, [](const char* name) { return SDL_getenv(name); });
        for (const std::string& w : warnings) SDL_Log("dcb: %s", w.c_str());
        SDL_Log("dcb: settings %s", path.string().c_str());
        if (trace_input_) SDL_Log("dcb: input trace on (keys, pad state, what the game reads)");
    }

    /// scale_mode hotkey: switch fit / integer scaling and remember the choice in settings.ini.
    void toggle_scale_mode() {
        const bool fit = settings_.display.scale_mode != ScaleMode::Fit;
        settings_.display.scale_mode = fit ? ScaleMode::Fit : ScaleMode::Integer;
        if (!settings_file_.set_and_save("display", "scale_mode", fit ? "fit" : "integer"))
            SDL_Log("dcb: cannot save %s", settings_file_.path().string().c_str());
    }

    /// Alt+Enter: flip fullscreen and remember the choice in settings.ini.
    void toggle_fullscreen() {
        const bool fullscreen = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) == 0;
        if (!SDL_SetWindowFullscreen(window_, fullscreen)) {
            SDL_Log("dcb: cannot change fullscreen: %s", SDL_GetError());
            return;
        }
        settings_.display.fullscreen = fullscreen;
        if (!settings_file_.set_and_save("display", "fullscreen", fullscreen ? "true" : "false"))
            SDL_Log("dcb: cannot save %s", settings_file_.path().string().c_str());
    }

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

    /// Pressed buttons (active HIGH) from the keyboard, per the [keyboard] bindings.
    /// Pressed buttons (active HIGH) from the keyboard, per the [keyboard] bindings. Key state comes
    /// from the key events, not SDL_GetKeyboardState (which stayed empty on some Wayland setups), and
    /// a quick tap still counts as held for kMinTapFrames frames.
    uint16_t read_keyboard() {
        // Alt+Enter toggles fullscreen; don't also press whatever Enter is bound to.
        const bool alt = (SDL_GetModState() & SDL_KMOD_ALT) != 0;
        uint16_t pressed = 0;
        for (size_t i = 0; i < kPadButtonCount; ++i) {
            for (const int code : settings_.keyboard[i]) {
                if (code < 0 || code >= static_cast<int>(SDL_SCANCODE_COUNT)) continue;
                if (!key_held_[static_cast<size_t>(code)] && key_tap_frames_[static_cast<size_t>(code)] == 0) continue;
                if (alt && is_enter(static_cast<SDL_Scancode>(code))) continue;
                pressed = static_cast<uint16_t>(pressed | kPadButtons[i].bit);
                break;
            }
        }
        for (uint8_t& f : key_tap_frames_) f = f > 0 ? static_cast<uint8_t>(f - 1) : f;
        return pressed;
    }

    /// Pressed buttons (active HIGH) from the open gamepad, per the [gamepad] bindings.
    uint16_t read_gamepad() const {
        if (gamepad_ == nullptr) return 0;
        uint16_t pressed = 0;
        for (size_t i = 0; i < kPadButtonCount; ++i) {
            for (const int code : settings_.gamepad[i]) {
                bool down = false;
                if (is_gamepad_axis(code)) {
                    const int axis = gamepad_axis_of(code);
                    down = gamepad_axis_pressed(SDL_GetGamepadAxis(gamepad_, static_cast<SDL_GamepadAxis>(axis)),
                                                gamepad_axis_negative(code), sdl3_axis_is_trigger(axis),
                                                settings_.stick_deadzone);
                } else {
                    down = SDL_GetGamepadButton(gamepad_, static_cast<SDL_GamepadButton>(code));
                }
                if (down) {
                    pressed = static_cast<uint16_t>(pressed | kPadButtons[i].bit);
                    break;
                }
            }
        }
        return pressed;
    }

    /// Destination rectangle per [display] scale_mode / aspect, centred in the output.
    SDL_FRect output_rect() const {
        int ow = 0, oh = 0;
        if (!SDL_GetRenderOutputSize(renderer_, &ow, &oh) || ow <= 0 || oh <= 0) {
            ow = kBaseWidth * settings_.display.scale;
            oh = kBaseHeight * settings_.display.scale;
        }
        const OutputRect r = compute_output_rect(ow, oh, settings_.display.scale_mode, settings_.display.aspect);
        return SDL_FRect{static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w),
                         static_cast<float>(r.h)};
    }

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    SDL_Texture* movie_texture_ = nullptr;  ///< native movie frames (created at the movie's size)
    int movie_w_ = 0, movie_h_ = 0;
    SDL_AudioStream* audio_ = nullptr;
    SDL_Gamepad* gamepad_ = nullptr;
    std::vector<uint32_t> pixels_;
    std::vector<int16_t> scaled_;  ///< volume-scaled audio scratch
    Settings settings_;
    SettingsFile settings_file_;
    uint16_t buttons_ = 0xFFFF;
    int blank_frames_ = 0;  ///< consecutive frames with the display off
    bool overlay_visible_ = false;
    /// DCB_TRACE_INPUT: log key presses and the pad state they produce.
    const bool trace_input_ = std::getenv("DCB_TRACE_INPUT") != nullptr;
    bool any_press_ = false;
    uint32_t commands_ = 0;  ///< HostCommand bits since take_commands()
    bool paused_ = false;
    trainer::Trainer* trainer_ = nullptr;  ///< [hotkeys] trainer panel (owned by the host loop)
    std::array<bool, SDL_SCANCODE_COUNT> key_held_{};    ///< keys down now (from key events)
    /// Frames a key still counts as pressed after going down, so a tap shorter than the game's own
    /// pad sampling interval is not lost.
    static constexpr uint8_t kMinTapFrames = 4;
    std::array<uint8_t, SDL_SCANCODE_COUNT> key_tap_frames_{};  ///< a key / gamepad button went down since take_any_press()
    FrameStats stats_;
    bool quit_ = false;
};

}  // namespace

std::unique_ptr<Platform> make_sdl3(const char* title, const std::filesystem::path& settings_path) {
    auto platform = std::make_unique<Sdl3>();
    if (!platform->init(title, settings_path)) return nullptr;
    return platform;
}

std::unique_ptr<Platform> make_sdl3(const char* title) {
    const auto exists = [](const std::filesystem::path& p) {
        std::error_code ec;
        return std::filesystem::is_regular_file(p, ec);
    };
    return make_sdl3(title, resolve_settings_path(current_settings_locations(), exists));
}

}  // namespace platform

#endif  // DCB_HAS_SDL3
