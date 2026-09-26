// SDL3 side of the trainer panel: keyboard / text-input routing and drawing with SDL's 8x8
// debug font. The panel logic itself lives in trainer.cpp (no SDL).

#if defined(DCB_HAS_SDL3)

#include "sdl3_trainer.hpp"

#include <algorithm>
#include <cmath>

namespace platform {

namespace {

bool is_bound(const std::vector<int>& keys, SDL_Scancode sc) {
    return std::find(keys.begin(), keys.end(), static_cast<int>(sc)) != keys.end();
}

void set_text_input(SDL_Window* window, bool on) {
    if (window == nullptr) return;
    if (on) SDL_StartTextInput(window); else SDL_StopTextInput(window);
}

}  // namespace

bool trainer_handle_event(trainer::Trainer& t, SDL_Window* window, const SDL_Event& ev,
                          const std::vector<int>& toggle_keys) {
    if (ev.type == SDL_EVENT_TEXT_INPUT) {
        if (!t.is_open()) return false;
        t.text(ev.text.text != nullptr ? ev.text.text : "");
        return true;
    }
    if (ev.type != SDL_EVENT_KEY_DOWN) return false;
    const SDL_Scancode sc = ev.key.scancode;
    if (is_bound(toggle_keys, sc)) {
        if (!ev.key.repeat) {
            t.set_open(!t.is_open());
            set_text_input(window, t.is_open());
        }
        return true;
    }
    if (!t.is_open()) return false;
    const bool enter = sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER;
    if (enter && (ev.key.mod & SDL_KMOD_ALT) != 0) return false;  // Alt+Enter: fullscreen
    using trainer::Key;
    switch (sc) {
    case SDL_SCANCODE_UP: t.key(Key::Up); break;
    case SDL_SCANCODE_DOWN: t.key(Key::Down); break;
    case SDL_SCANCODE_LEFT: t.key(Key::Left); break;
    case SDL_SCANCODE_RIGHT: t.key(Key::Right); break;
    case SDL_SCANCODE_PAGEUP: t.key(Key::PageUp); break;
    case SDL_SCANCODE_PAGEDOWN: t.key(Key::PageDown); break;
    case SDL_SCANCODE_HOME: t.key(Key::Home); break;
    case SDL_SCANCODE_END: t.key(Key::End); break;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: t.key(Key::Enter); break;
    case SDL_SCANCODE_BACKSPACE: t.key(Key::Backspace); break;
    case SDL_SCANCODE_DELETE: t.key(Key::Delete); break;
    case SDL_SCANCODE_TAB: t.key(Key::Tab); break;
    case SDL_SCANCODE_ESCAPE: t.key(Key::Close); break;
    default: break;  // characters arrive as SDL_EVENT_TEXT_INPUT
    }
    if (!t.is_open()) set_text_input(window, false);
    return true;  // the open panel owns the keyboard
}

void trainer_draw(SDL_Renderer* renderer, const trainer::Trainer& t) {
    constexpr int kCharW = 8, kLineH = 10, kPad = 8;
    int ww = 0, wh = 0;
    if (!SDL_GetRenderOutputSize(renderer, &ww, &wh) || ww <= 0 || wh <= 0) return;
    // Largest scale, in half steps, at which the full panel fits; below that the panel shrinks to
    // the characters that fit at scale 1 (8x8 glyphs stay legible down to a 320x240 window).
    const float need_w = static_cast<float>(trainer::kPanelCols * kCharW + 2 * kPad);
    const float need_h = static_cast<float>(trainer::kPanelRows * kLineH + 2 * kPad);
    const float fit = std::min(static_cast<float>(ww) / need_w, static_cast<float>(wh) / need_h);
    const float scale = std::max(1.0f, std::floor(fit * 2.0f) / 2.0f);
    const int vw = static_cast<int>(static_cast<float>(ww) / scale), vh = static_cast<int>(static_cast<float>(wh) / scale);
    const int cols = std::clamp((vw - 2 * kPad) / kCharW, 1, trainer::kPanelCols);
    const int rows = std::clamp((vh - 2 * kPad) / kLineH, 1, trainer::kPanelRows);
    const std::vector<trainer::Line> lines = t.render(cols, rows);

    SDL_SetRenderScale(renderer, scale, scale);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    const float pw = static_cast<float>(cols * kCharW + 2 * kPad), ph = static_cast<float>(rows * kLineH + 2 * kPad);
    const float x0 = std::max(0.0f, (static_cast<float>(vw) - pw) / 2.0f);
    const float y0 = std::max(0.0f, (static_cast<float>(vh) - ph) / 2.0f);
    const SDL_FRect panel{x0, y0, pw, ph};
    SDL_SetRenderDrawColor(renderer, 8, 12, 24, 225);
    SDL_RenderFillRect(renderer, &panel);
    SDL_SetRenderDrawColor(renderer, 90, 120, 200, 255);
    SDL_RenderRect(renderer, &panel);

    for (size_t i = 0; i < lines.size(); ++i) {
        const float x = x0 + static_cast<float>(kPad);
        const float y = y0 + static_cast<float>(kPad) + static_cast<float>(i) * static_cast<float>(kLineH);
        switch (lines[i].style) {
        case trainer::Style::Selected: {
            const SDL_FRect bar{x - 2.0f, y - 1.0f, static_cast<float>(cols * kCharW) + 4.0f, static_cast<float>(kLineH)};
            SDL_SetRenderDrawColor(renderer, 50, 80, 150, 255);
            SDL_RenderFillRect(renderer, &bar);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            break;
        }
        case trainer::Style::Title: SDL_SetRenderDrawColor(renderer, 255, 220, 120, 255); break;
        case trainer::Style::Dim: SDL_SetRenderDrawColor(renderer, 150, 155, 170, 255); break;
        case trainer::Style::Good: SDL_SetRenderDrawColor(renderer, 120, 255, 120, 255); break;
        case trainer::Style::Error: SDL_SetRenderDrawColor(renderer, 255, 110, 110, 255); break;
        case trainer::Style::Normal: SDL_SetRenderDrawColor(renderer, 230, 230, 230, 255); break;
        }
        SDL_RenderDebugText(renderer, x, y, lines[i].text.c_str());
    }
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
}

}  // namespace platform

#endif  // DCB_HAS_SDL3
