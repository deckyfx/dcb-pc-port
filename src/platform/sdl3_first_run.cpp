// First run without game data (SDL3 builds): explain that a dump of the player's own disc is
// needed, let them pick it with the system file dialog, import it with a progress window, and
// hand the imported tree back to the boot sequence. Self-contained: it brings SDL up and shuts it
// down again before the game's own window (sdl3.cpp) is created.
//
// DCB_IMPORT_IMAGE=<path> skips the explanation and the file dialog (automated tests of the flow).

#if defined(DCB_HAS_SDL3)

#include "first_run.hpp"

#include "cdrom/importer.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

namespace platform {

namespace fs = std::filesystem;
namespace imp = hle::import;

namespace {

constexpr int kWidth = 640, kHeight = 360;
constexpr float kScale = 2.0f;  ///< debug text is 8x8; draw at 2x (320x180 logical)
constexpr const char* kTitle = "Digimon World: Digital Card Arena - first run";

fs::path from_utf8(const char* s) {
    const std::string str(s);
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(str.data()), str.size()));
}

std::string to_utf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

class FirstRun {
public:
    FirstRun(std::string serial, fs::path dest_root) : serial_(std::move(serial)), dest_root_(std::move(dest_root)) {}

    ~FirstRun() {
        if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
        if (window_ != nullptr) SDL_DestroyWindow(window_);
        if (sdl_) SDL_Quit();
    }

    fs::path run() {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            SDL_Log("dcb: first run: no display (%s)", SDL_GetError());
            return {};
        }
        sdl_ = true;
        if (!SDL_CreateWindowAndRenderer(kTitle, kWidth, kHeight, 0, &window_, &renderer_)) {
            SDL_Log("dcb: first run: cannot create a window: %s", SDL_GetError());
            return {};
        }
        SDL_SetRenderScale(renderer_, kScale, kScale);
        SDL_SetRenderVSync(renderer_, 1);
        draw_text({"Digital Card Arena PC port", "", "No game data yet."});

        std::error_code ec;
        const std::string dest = to_utf8(fs::absolute(dest_root_ / serial_, ec));
        const char* preset = std::getenv("DCB_IMPORT_IMAGE");
        for (bool first = true;; first = false) {
            fs::path image;
            if (preset != nullptr) {
                if (!first) return {};  // automated run: one attempt
                image = from_utf8(preset);
            } else {
                if (!ask_intro(dest)) return {};
                image = pick_file();
                if (quit_) return {};
                if (image.empty()) continue;  // dialog cancelled: explain again
            }
            const Outcome out = import(image);
            if (quit_) return {};
            if (out.ok && out.serial == serial_) return out.dir;
            if (out.ok || out.code == imp::ErrorCode::AlreadyExists) {
                const char* title = imp::known_title(out.serial);
                message(SDL_MESSAGEBOX_WARNING, "Different version of the game",
                        "That disc is " + std::string(title ? title : out.serial) + ", " + out.serial + ".\n\n" +
                            "Its files are kept in " + to_utf8(fs::absolute(dest_root_ / out.serial, ec)) +
                            " for a planned English option, but this version plays the Japanese disc (" + serial_ +
                            ").\n\nPlease choose a dump of the Japanese disc.");
            } else if (out.code != imp::ErrorCode::Cancelled) {
                message(SDL_MESSAGEBOX_ERROR, "Import failed", "The disc image could not be imported:\n\n" + out.error);
            }
        }
    }

private:
    struct Outcome {
        bool ok = false;
        imp::ErrorCode code = imp::ErrorCode::Io;
        std::string error;
        std::string serial;
        fs::path dir;
    };

    std::string serial_;
    fs::path dest_root_;
    bool sdl_ = false;
    bool quit_ = false;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;

    void message(SDL_MessageBoxFlags flags, const char* title, const std::string& text) {
        if (!SDL_ShowSimpleMessageBox(flags, title, text.c_str(), window_))
            std::fprintf(stderr, "[import] %s: %s\n", title, text.c_str());
    }

    /// Explain what is needed; true = "Choose disc image...", false = quit.
    bool ask_intro(const std::string& dest) {
        const std::string text =
            "This PC port does not include the game. It runs from a copy of your own disc:\n\n"
            "    " + std::string(imp::known_title(serial_) ? imp::known_title(serial_) : "the game") + ", " +
            serial_ + "\n\n"
            "dumped as a raw .cue/.bin image (2352 bytes per sector, e.g. made with ImgBurn or cdrdao; "
            ".iso/.chd/.pbp files do not work).\n\n"
            "Choose the .cue file (or the .bin). Its game files are copied once into\n\n"
            "    " + dest + "\n\n"
            "(about 250 MB). After that the disc image is no longer needed.";
        const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"},
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Choose disc image..."},
        };
        const SDL_MessageBoxData data = {SDL_MESSAGEBOX_INFORMATION, window_, "Game data needed", text.c_str(),
                                         SDL_arraysize(buttons), buttons, nullptr};
        int button = 0;
        if (!SDL_ShowMessageBox(&data, &button)) {
            SDL_Log("dcb: first run: cannot show a message box: %s", SDL_GetError());
            return false;
        }
        return button == 1;
    }

    /// The system file dialog; empty if cancelled (quit_ set if the window was closed).
    fs::path pick_file() {
        struct Pick {
            std::mutex mutex;
            bool done = false;
            std::string path, error;
        } pick;
        static const SDL_DialogFileFilter filters[] = {
            {"PlayStation disc image (.cue, .bin)", "cue;bin"},
            {"All files", "*"},
        };
        SDL_ShowOpenFileDialog(
            [](void* user, const char* const* files, int) {
                auto* p = static_cast<Pick*>(user);
                std::lock_guard lock(p->mutex);
                if (files == nullptr)
                    p->error = SDL_GetError();
                else if (files[0] != nullptr)
                    p->path = files[0];
                p->done = true;
            },
            &pick, window_, filters, 2, nullptr, false);
        for (;;) {
            {
                std::lock_guard lock(pick.mutex);
                if (pick.done) break;
            }
            pump();
            draw_text({"Choose your disc image", "", "(.cue or .bin)"});
        }
        if (!pick.error.empty())
            message(SDL_MESSAGEBOX_ERROR, "File dialog", "The file dialog could not be opened: " + pick.error +
                                                             "\n\nImport from a terminal instead:\n    dcb --import <disc.cue>");
        if (!pick.error.empty()) quit_ = true;
        return pick.path.empty() ? fs::path() : from_utf8(pick.path.c_str());
    }

    /// Run the import on a worker thread while this thread shows its progress.
    Outcome import(const fs::path& image) {
        Outcome out;
        std::mutex mutex;
        imp::Progress progress;
        std::atomic<bool> cancel{false}, finished{false};
        std::thread worker([&] {
            try {
                // Keep an existing complete tree (it may hold replaced assets), replace a broken one.
                const imp::DiscInfo info = imp::identify(image);
                std::error_code ec;
                imp::Options options;
                options.overwrite = !fs::exists(dest_root_ / info.serial / "layout.txt", ec);
                options.progress = [&](const imp::Progress& p) {
                    std::lock_guard lock(mutex);
                    progress = p;
                    return !cancel.load();
                };
                const imp::Result r = imp::import_disc(image, dest_root_, options);
                out.ok = true;
                out.serial = r.disc.serial;
                out.dir = r.dir;
            } catch (const imp::ImportError& e) {
                out.code = e.code();
                out.error = e.what();
                if (e.code() == imp::ErrorCode::AlreadyExists) out.serial = imp::identify(image).serial;
            } catch (const std::exception& e) {
                out.error = e.what();
            }
            finished = true;
        });
        while (!finished) {
            pump();
            if (quit_) cancel = true;
            imp::Progress p;
            {
                std::lock_guard lock(mutex);
                p = progress;
            }
            draw_progress(p);
        }
        worker.join();
        return out;
    }

    void pump() {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit_ = true;
        SDL_Delay(10);
    }

    void begin_frame() {
        SDL_SetRenderDrawColor(renderer_, 16, 20, 36, 255);
        SDL_RenderClear(renderer_);
        SDL_SetRenderDrawColor(renderer_, 230, 230, 230, 255);
    }

    void draw_text(std::initializer_list<const char*> lines) {
        begin_frame();
        float y = 60.0f;
        for (const char* line : lines) {
            SDL_RenderDebugText(renderer_, 16.0f, y, line);
            y += 12.0f;
        }
        SDL_RenderPresent(renderer_);
    }

    void draw_progress(const imp::Progress& p) {
        const int pct = p.total ? static_cast<int>(p.done * 100 / p.total) : 0;
        begin_frame();
        char line[96];
        std::snprintf(line, sizeof line, "Importing... %d%%", pct);
        SDL_RenderDebugText(renderer_, 16.0f, 48.0f, line);
        // Bar: 288 x 10 logical pixels.
        const SDL_FRect frame = {16.0f, 64.0f, 288.0f, 10.0f};
        SDL_RenderRect(renderer_, &frame);
        SDL_SetRenderDrawColor(renderer_, 90, 170, 255, 255);
        const SDL_FRect bar = {18.0f, 66.0f, 284.0f * static_cast<float>(pct) / 100.0f, 6.0f};
        SDL_RenderFillRect(renderer_, &bar);
        SDL_SetRenderDrawColor(renderer_, 200, 200, 200, 255);
        std::snprintf(line, sizeof line, "%s %.24s", p.stage, p.item.c_str());
        SDL_RenderDebugText(renderer_, 16.0f, 84.0f, line);
        SDL_RenderDebugText(renderer_, 16.0f, 150.0f, "Close the window to cancel.");
        SDL_RenderPresent(renderer_);
    }
};

}  // namespace

fs::path sdl3_first_run(const std::string& serial, const fs::path& dest_root) {
    return FirstRun(serial, dest_root).run();
}

}  // namespace platform

#endif  // DCB_HAS_SDL3
