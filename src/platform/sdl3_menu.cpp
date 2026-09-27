// SDL3 side of the native pause menu: keyboard / gamepad routing and drawing
// with SDL's 8x8 debug font. The menu logic itself lives in menu.cpp (no SDL).

#if defined(DCB_HAS_SDL3)

#include "sdl3_menu.hpp"

#include <algorithm>
#include <cmath>

namespace platform {

namespace {

bool is_bound(const std::vector<int>& keys, SDL_Scancode sc) {
    return std::find(keys.begin(), keys.end(), static_cast<int>(sc)) != keys.end();
}

menu::Action gamepad_button(menu::Menu& m, SDL_GamepadButton b) {
    using menu::Pad;
    switch (b) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return m.pad(Pad::Up);
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return m.pad(Pad::Down);
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return m.pad(Pad::Left);
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return m.pad(Pad::Right);
    case SDL_GAMEPAD_BUTTON_SOUTH: return m.pad(Pad::South);
    case SDL_GAMEPAD_BUTTON_EAST: return m.pad(Pad::East);
    case SDL_GAMEPAD_BUTTON_START: return m.pad(Pad::Start);
    default: break;
    }
    return menu::Action::None;
}

}  // namespace

void menu_track_chord(const SDL_Event& ev, bool& start_held, bool& select_held) {
    if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || ev.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        const bool down = ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_START) start_held = down;
        if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) select_held = down;
    }
}

menu::Action menu_handle_event(menu::Menu& m, const SDL_Event& ev, const std::vector<int>& toggle_keys,
                               SDL_Gamepad* gamepad, bool& start_held, bool& select_held) {
    using menu::Action;
    using menu::Key;
    menu_track_chord(ev, start_held, select_held);
    // Start+Select opens the menu (configurable button support would need a
    // gamepad hotkey table; Start+Select is the fixed, documented chord).
    if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && ev.gbutton.button == SDL_GAMEPAD_BUTTON_START && select_held) {
        m.set_open(!m.is_open());
        return Action::None;
    }
    if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && ev.gbutton.button == SDL_GAMEPAD_BUTTON_BACK && start_held) {
        m.set_open(!m.is_open());
        return Action::None;
    }
    if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && m.is_open()) {
        if (gamepad != nullptr && ev.gdevice.which != SDL_GetGamepadID(gamepad)) return Action::None;
        return gamepad_button(m, static_cast<SDL_GamepadButton>(ev.gbutton.button));
    }
    if (ev.type != SDL_EVENT_KEY_DOWN) return Action::None;
    const SDL_Scancode sc = ev.key.scancode;
    if (!ev.key.repeat && (is_bound(toggle_keys, sc) || sc == SDL_SCANCODE_ESCAPE)) {
        m.set_open(!m.is_open());
        // Closing via Escape on the main page means Resume; opening never acts.
        if (!m.is_open() && !m.confirming_quit()) return Action::Resume;
        return Action::None;
    }
    if (!m.is_open()) return Action::None;
    switch (sc) {
    case SDL_SCANCODE_UP: return m.key(Key::Up);
    case SDL_SCANCODE_DOWN: return m.key(Key::Down);
    case SDL_SCANCODE_LEFT: return m.key(Key::Left);
    case SDL_SCANCODE_RIGHT: return m.key(Key::Right);
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: return m.key(Key::Enter);
    default: break;
    }
    return Action::None;
}

void menu_draw(SDL_Renderer* renderer, const menu::Menu& m) {
    constexpr int kCharW = 8, kLineH = 10, kPad = 8;
    int ww = 0, wh = 0;
    if (!SDL_GetRenderOutputSize(renderer, &ww, &wh) || ww <= 0 || wh <= 0) return;
    const float need_w = static_cast<float>(menu::kMenuCols * kCharW + 2 * kPad);
    const float need_h = static_cast<float>(menu::kMenuRows * kLineH + 2 * kPad);
    const float fit = std::min(static_cast<float>(ww) / need_w, static_cast<float>(wh) / need_h);
    const float scale = std::max(1.0f, std::floor(fit * 2.0f) / 2.0f);
    const int vw = static_cast<int>(static_cast<float>(ww) / scale), vh = static_cast<int>(static_cast<float>(wh) / scale);
    const int cols = std::clamp((vw - 2 * kPad) / kCharW, 1, menu::kMenuCols);
    const int rows = std::clamp((vh - 2 * kPad) / kLineH, 1, menu::kMenuRows);
    const std::vector<menu::Line> lines = m.render(cols, rows);

    SDL_SetRenderScale(renderer, scale, scale);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    const float pw = static_cast<float>(cols * kCharW + 2 * kPad), ph = static_cast<float>(rows * kLineH + 2 * kPad);
    const float x0 = std::max(0.0f, (static_cast<float>(vw) - pw) / 2.0f);
    const float y0 = std::max(0.0f, (static_cast<float>(vh) - ph) / 2.0f);
    const SDL_FRect panel{x0, y0, pw, ph};
    SDL_SetRenderDrawColor(renderer, 8, 12, 24, 228);
    SDL_RenderFillRect(renderer, &panel);
    SDL_SetRenderDrawColor(renderer, 90, 120, 200, 255);
    SDL_RenderRect(renderer, &panel);

    // Slot thumbnail on the States page: the display capture next to the rows.
    int thumb_w = 0, thumb_h = 0;
    const uint8_t* thumb_rgb = nullptr;
    SDL_Texture* thumb_tex = nullptr;
    if (m.selected_thumbnail(thumb_w, thumb_h, thumb_rgb) && thumb_rgb != nullptr && thumb_w > 0 && thumb_h > 0) {
        thumb_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STATIC, thumb_w, thumb_h);
        if (thumb_tex != nullptr) SDL_UpdateTexture(thumb_tex, nullptr, thumb_rgb, thumb_w * 3);
    }
    const float thumb_scale =
        thumb_tex != nullptr ? std::min(1.0f, std::min(192.0f / static_cast<float>(thumb_w),
                                                       144.0f / static_cast<float>(thumb_h)))
                             : 1.0f;
    const float thumb_dw = thumb_tex != nullptr ? static_cast<float>(thumb_w) * thumb_scale : 0.0f;
    const float thumb_dh = thumb_tex != nullptr ? static_cast<float>(thumb_h) * thumb_scale : 0.0f;
    const float thumb_x = x0 + pw - thumb_dw - static_cast<float>(kPad);
    const float thumb_y = y0 + static_cast<float>(kPad);
    if (thumb_tex != nullptr) {
        const SDL_FRect dst{thumb_x, thumb_y, thumb_dw, thumb_dh};
        SDL_RenderTexture(renderer, thumb_tex, nullptr, &dst);
        SDL_SetRenderDrawColor(renderer, 90, 120, 200, 255);
        SDL_RenderRect(renderer, &dst);
    }

    for (size_t i = 0; i < lines.size(); ++i) {
        const float x = x0 + static_cast<float>(kPad);
        const float y = y0 + static_cast<float>(kPad) + static_cast<float>(i) * static_cast<float>(kLineH);
        switch (lines[i].style) {
        case menu::Style::Selected: {
            const SDL_FRect bar{x - 2.0f, y - 1.0f, static_cast<float>(cols * kCharW) + 4.0f, static_cast<float>(kLineH)};
            SDL_SetRenderDrawColor(renderer, 50, 80, 150, 255);
            SDL_RenderFillRect(renderer, &bar);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            break;
        }
        case menu::Style::Title: SDL_SetRenderDrawColor(renderer, 255, 220, 120, 255); break;
        case menu::Style::Dim: SDL_SetRenderDrawColor(renderer, 150, 155, 170, 255); break;
        case menu::Style::Good: SDL_SetRenderDrawColor(renderer, 120, 255, 120, 255); break;
        case menu::Style::Error: SDL_SetRenderDrawColor(renderer, 255, 110, 110, 255); break;
        case menu::Style::Normal: SDL_SetRenderDrawColor(renderer, 230, 230, 230, 255); break;
        }
        SDL_RenderDebugText(renderer, x, y, lines[i].text.c_str());
    }
    if (thumb_tex != nullptr) SDL_DestroyTexture(thumb_tex);
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
}

}  // namespace platform

#endif  // DCB_HAS_SDL3
