// First run of the public download (SDL3 builds): explain what is needed, let the player pick
// their two disc images with the system file dialog (the Japanese one, then the North American
// one), check each against redump.org and import it with a progress window, optionally take a
// folder of community fixes, build the English data (patch::build_all), and hand the imported
// game back to the boot sequence. Only the steps setup_needs() asks for are shown. Self-contained:
// it brings SDL up and shuts it down again before the game's own window (sdl3.cpp) is created.
//
// Everything can be quit at any point and is picked up again on the next start: imports are
// all-or-nothing (hle::import), and the English build leaves an unfinished stamp until it is done
// (build_english()). A failed build is shown and the game is not started.
//
// Automated tests of the flow skip the explanations and dialogs (one attempt each):
//   DCB_IMPORT_IMAGE=<path>     the Japanese disc image
//   DCB_IMPORT_US_IMAGE=<path>  the North American disc image
//   DCB_SETUP_FIXES=<dir>       the fixes folder (unset: none)

#if defined(DCB_HAS_SDL3)

#include "first_run.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
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

std::string title_of(const std::string& serial) {
    const char* title = imp::known_title(serial);
    return std::string(title ? title : "the game") + ", " + serial;
}

/// What the worker thread reports, for the progress window.
struct Shown {
    std::string stage, item;
    uint64_t done = 0, total = 0;
};
/// Report progress from the worker; false = the player cancelled.
using Report = std::function<bool(const Shown&)>;

class FirstRun {
public:
    explicit FirstRun(SetupNeeds needs) : needs_(std::move(needs)) {}

    ~FirstRun() {
        if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
        if (window_ != nullptr) SDL_DestroyWindow(window_);
        if (sdl_) SDL_Quit();
    }

    FirstRunResult run() {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            SDL_Log("dcb: first run: no display (%s)", SDL_GetError());
            return {FirstRunResult::NoDisplay, {}};
        }
        sdl_ = true;
        if (!SDL_CreateWindowAndRenderer(kTitle, kWidth, kHeight, 0, &window_, &renderer_)) {
            SDL_Log("dcb: first run: cannot create a window: %s", SDL_GetError());
            return {FirstRunResult::NoDisplay, {}};
        }
        SDL_SetRenderScale(renderer_, kScale, kScale);
        SDL_SetRenderVSync(renderer_, 1);
        draw_text({"Digital Card Arena PC port", "", "First-run setup."});

        const FirstRunResult quit = {FirstRunResult::Quit, {}};
        if (!automated() && !ask_intro()) return quit;
        if (needs_.need_jp && !obtain(needs_.serial, std::getenv("DCB_IMPORT_IMAGE"))) return quit;
        if (needs_.need_us && !obtain(kEnglishSerial, std::getenv("DCB_IMPORT_US_IMAGE"))) return quit;
        if (needs_.need_english && !build()) return quit;
        return {FirstRunResult::Done, needs_.jp_dump()};
    }

private:
    SetupNeeds needs_;
    bool sdl_ = false;
    bool quit_ = false;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;

    static bool automated() { return std::getenv("DCB_IMPORT_IMAGE") != nullptr; }

    bool have(const std::string& serial) const {
        std::error_code ec;
        return fs::is_regular_file(needs_.dump_root() / serial / "layout.txt", ec);
    }

    void message(SDL_MessageBoxFlags flags, const char* title, const std::string& text) {
        if (!SDL_ShowSimpleMessageBox(flags, title, text.c_str(), window_))
            std::fprintf(stderr, "[setup] %s: %s\n", title, text.c_str());
    }

    /// A message box with two buttons; true = `yes` (Return), false = `no` (Escape, or no box).
    bool ask(const char* title, const std::string& text, const char* no, const char* yes) {
        const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, no},
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, yes},
        };
        const SDL_MessageBoxData data = {SDL_MESSAGEBOX_INFORMATION, window_, title, text.c_str(),
                                         SDL_arraysize(buttons), buttons, nullptr};
        int button = 0;
        if (!SDL_ShowMessageBox(&data, &button)) {
            SDL_Log("dcb: first run: cannot show a message box: %s", SDL_GetError());
            return false;
        }
        return button == 1;
    }

    /// Explain what is needed; true = continue, false = quit.
    bool ask_intro() {
        std::error_code ec;
        const std::string dest = to_utf8(fs::absolute(needs_.assets, ec));
        std::string text = "This PC port does not include the game. It runs from copies of your own discs";
        if (needs_.need_english) {
            text +=
                ":\n\n"
                "    " + title_of(needs_.serial) + "\n        the game itself\n"
                "    " + title_of(kEnglishSerial) + "\n        the English text and art\n\n";
        } else {
            text += ":\n\n    " + title_of(needs_.serial) + "\n\n";
        }
        text +=
            "each dumped as a raw .cue/.bin image (2352 bytes per sector, e.g. made with ImgBurn, cdrdao or "
            "redumper; .iso/.chd/.pbp files do not work). Each image is checked against the redump.org "
            "database, then its game files are copied once into\n\n"
            "    " + dest + "\n\n"
            "(about 250 MB per disc). After that the disc images are no longer needed.";
        if (!needs_.verify) text += "\n\n(Verification is off: --no-verify / DCB_NO_VERIFY.)";
        return ask("Game data needed", text, "Quit", "Continue");
    }

    /// Get `serial` imported: ask for its image until it is (true), or the player quits (false).
    /// Another known disc chosen instead is imported too when the setup needs it.
    bool obtain(const std::string& serial, const char* preset) {
        for (bool first = true; !have(serial); first = false) {
            fs::path image;
            if (preset != nullptr || automated()) {
                if (!first || preset == nullptr) return false;  // automated run: one attempt
                image = from_utf8(preset);
            } else {
                const bool jp = serial == needs_.serial;
                const bool both = needs_.need_jp && needs_.need_us;
                const std::string text =
                    std::string(both ? (jp ? "Step 1 of 2: " : "Step 2 of 2: ") : "") +
                    (jp ? "the Japanese disc" : "the North American disc") + "\n\n    " + title_of(serial) +
                    "\n\nChoose its .cue file (or the .bin).";
                if (!ask(jp ? "Japanese disc" : "North American disc", text, "Quit", "Choose disc image...")) return false;
                image = pick_file(jp ? "Choose the Japanese disc image" : "Choose the North American disc image");
                if (quit_) return false;
                if (image.empty()) continue;  // dialog cancelled: ask again
            }
            Imported got;
            const std::string error = work("Checking", [&](const Report& report) {
                got = import_checked(image, needs_.dump_root(), needs_.verify, [&](const imp::Progress& p) {
                    return report({p.stage, p.item, p.done, p.total});
                });
            });
            if (quit_) return false;
            if (!error.empty()) {
                message(SDL_MESSAGEBOX_ERROR, "This disc image cannot be used", error);
                continue;
            }
            if (got.serial != serial) {
                std::error_code ec;
                message(SDL_MESSAGEBOX_WARNING, "A different disc",
                        "That is " + title_of(got.serial) + ".\n\n" +
                            (got.serial == needs_.serial || got.serial == kEnglishSerial
                                 ? "It is needed too and has been kept in " + to_utf8(fs::absolute(got.dir, ec)) + ". "
                                 : std::string()) +
                            "Please choose " + title_of(serial) + ".");
            }
        }
        return true;
    }

    /// The optional fixes folder; empty = none. Sets quit_ if the window was closed.
    fs::path ask_fixes() {
        if (automated()) {
            const char* dir = std::getenv("DCB_SETUP_FIXES");
            return dir ? from_utf8(dir) : fs::path();
        }
        const std::string text =
            "Optional: community fixes.\n\n"
            "Fan-made fixes for the English data (.xdelta files) can be applied now. They are made by other "
            "players, are not part of this program and are never shipped with it; if you have some, put them "
            "in one folder and choose it.\n\n"
            "Most players skip this step. Fixes can also be applied later from a terminal:\n"
            "    dcb --setup <jp.cue> <us.cue> --fixes <folder>";
        if (!ask("Community fixes (optional)", text, "Skip", "Choose folder...")) return {};
        return pick(true, "Choose the folder with the .xdelta fixes");
    }

    /// Build the English data; true when done, false when quit or failed (already shown).
    bool build() {
        const fs::path fixes = ask_fixes();
        if (quit_) return false;
        for (;;) {
            const std::string error = work("Building the English version", [&](const Report& report) {
                build_english(needs_, fixes, [&](const patch::Progress& p) {
                    return report({p.stage, std::string(), p.done, p.total});
                });
            });
            if (quit_) return false;
            if (error.empty()) return true;
            if (automated() ||
                !ask("The English version could not be built",
                     "Building the English data failed:\n\n" + error +
                         "\n\nThe game is not started with incomplete data. Try again, or quit (the setup "
                         "continues on the next start).",
                     "Quit", "Try again")) {
                message(SDL_MESSAGEBOX_ERROR, "Setup not finished", "Building the English data failed:\n\n" + error);
                return false;
            }
        }
    }

    /// The system file (or folder) dialog; empty if cancelled (quit_ set if the dialog failed).
    fs::path pick(bool folder, const char* prompt) {
        struct Pick {
            std::mutex mutex;
            bool done = false;
            std::string path, error;
        } result;
        static const SDL_DialogFileFilter filters[] = {
            {"PlayStation disc image (.cue, .bin)", "cue;bin"},
            {"All files", "*"},
        };
        const SDL_DialogFileCallback callback = [](void* user, const char* const* files, int) {
            auto* p = static_cast<Pick*>(user);
            std::lock_guard lock(p->mutex);
            if (files == nullptr)
                p->error = SDL_GetError();
            else if (files[0] != nullptr)
                p->path = files[0];
            p->done = true;
        };
        if (folder)
            SDL_ShowOpenFolderDialog(callback, &result, window_, nullptr, false);
        else
            SDL_ShowOpenFileDialog(callback, &result, window_, filters, 2, nullptr, false);
        for (;;) {
            {
                std::lock_guard lock(result.mutex);
                if (result.done) break;
            }
            pump();
            draw_text({prompt});
        }
        if (!result.error.empty()) {
            message(SDL_MESSAGEBOX_ERROR, "File dialog",
                    "The file dialog could not be opened: " + result.error +
                        "\n\nSet the game up from a terminal instead:\n    dcb --setup <jp.cue> <us.cue>");
            quit_ = true;
        }
        return result.path.empty() ? fs::path() : from_utf8(result.path.c_str());
    }
    fs::path pick_file(const char* prompt) { return pick(false, prompt); }

    /// Run `job(report)` on a worker thread while this thread shows its progress. Closing the
    /// window cancels (quit_ set; the job sees report() return false). Returns the job's error
    /// message, empty on success or cancellation.
    template <typename Job>
    std::string work(const char* heading, Job job) {
        std::mutex mutex;
        Shown shown;
        std::string error;
        std::atomic<bool> cancel{false}, finished{false};
        std::thread worker([&] {
            try {
                job(Report([&](const Shown& s) {
                    std::lock_guard lock(mutex);
                    shown = s;
                    return !cancel.load();
                }));
            } catch (const imp::ImportError& e) {
                if (e.code() != imp::ErrorCode::Cancelled) error = e.what();
            } catch (const patch::Cancelled&) {
            } catch (const std::exception& e) {
                error = e.what();
            }
            finished = true;
        });
        while (!finished) {
            pump();
            if (quit_) cancel = true;
            Shown s;
            {
                std::lock_guard lock(mutex);
                s = shown;
            }
            draw_progress(heading, s);
        }
        worker.join();
        return quit_ ? std::string() : error;
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

    void draw_progress(const char* heading, const Shown& p) {
        const int pct = p.total ? static_cast<int>(p.done * 100 / p.total) : 0;
        begin_frame();
        char line[96];
        std::snprintf(line, sizeof line, "%s... %d%%", heading, pct);
        SDL_RenderDebugText(renderer_, 16.0f, 48.0f, line);
        // Bar: 288 x 10 logical pixels.
        const SDL_FRect frame = {16.0f, 64.0f, 288.0f, 10.0f};
        SDL_RenderRect(renderer_, &frame);
        SDL_SetRenderDrawColor(renderer_, 90, 170, 255, 255);
        const SDL_FRect bar = {18.0f, 66.0f, 284.0f * static_cast<float>(pct) / 100.0f, 6.0f};
        SDL_RenderFillRect(renderer_, &bar);
        SDL_SetRenderDrawColor(renderer_, 200, 200, 200, 255);
        std::snprintf(line, sizeof line, "%.18s %.18s", p.stage.c_str(), p.item.c_str());
        SDL_RenderDebugText(renderer_, 16.0f, 84.0f, line);
        SDL_RenderDebugText(renderer_, 16.0f, 150.0f, "Close the window to cancel.");
        SDL_RenderPresent(renderer_);
    }
};

}  // namespace

FirstRunResult sdl3_first_run(const SetupNeeds& needs) { return FirstRun(needs).run(); }

}  // namespace platform

#endif  // DCB_HAS_SDL3
