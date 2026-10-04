#include "first_run.hpp"

#include "cdrom/disc.hpp"
#include "cdrom/importer.hpp"
#include "settings.hpp"
#include "vfs/payload.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace platform {

namespace fs = std::filesystem;
namespace imp = hle::import;

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: dcb --import <disc.cue|disc.bin> [dest-dir] [--force]\n"
                 "  Imports a dump of your own disc (raw .cue/.bin, 2352 bytes per sector) into\n"
                 "  <dest-dir>/<serial>/ (default: assets/dump/ in the current directory). The game then\n"
                 "  runs from those files; the disc image is no longer needed.\n"
                 "  --force  replace an existing <dest-dir>/<serial>/\n");
}

void setup_usage() {
    std::fprintf(stderr,
                 "usage: dcb --setup <jp.cue|jp.bin> <us.cue|us.bin> [--fixes DIR] [--no-verify] [--force]\n"
                 "  Sets the game up from dumps of your own two discs (raw .cue/.bin, 2352 bytes per sector):\n"
                 "    Digimon World: Digital Card Arena (Japan, SLPS-03101)      the game that runs\n"
                 "    Digimon Digital Card Battle (North America, SLUS-01328)    the English text and art\n"
                 "  Both are checked against redump.org, imported into assets/dump/ and the English data is\n"
                 "  built into assets/, all in the current directory. The disc images are not needed afterwards.\n"
                 "  --fixes DIR  community fixes (.xdelta files) to apply; optional\n"
                 "  --no-verify  skip the redump.org check (also DCB_NO_VERIFY=1), for modified images\n"
                 "  --force      build even over English data that this setup did not make\n");
}

/// Terminal progress: one line, rewritten when the percentage changes.
class ConsoleProgress {
public:
    explicit ConsoleProgress(const char* tag) : tag_(tag) {}
    void show(const std::string& stage, uint64_t done, uint64_t total) {
        const int pct = total ? static_cast<int>(done * 100 / total) : 0;
        if (pct == shown_ && stage == stage_) return;
        shown_ = pct;
        stage_ = stage;
        std::fprintf(stderr, "\r[%s] %3d%%  %-28.28s", tag_, pct, stage.c_str());
        std::fflush(stderr);
    }
    void end() {
        if (shown_ >= 0) std::fprintf(stderr, "\n");
        shown_ = -1;
        stage_.clear();
    }

private:
    const char* tag_;
    int shown_ = -1;
    std::string stage_;
};

bool is_import(const fs::path& dir) {
    std::error_code ec;
    return fs::is_regular_file(dir / "layout.txt", ec);
}

/// Where English data made by anyone (this setup or the Python pipeline) is looked for: the same
/// places the game reads it from (main.cpp: the executable's assets/, then the current one's).
std::vector<fs::path> english_roots(const fs::path& assets) {
    std::vector<fs::path> roots = {assets, fs::path(hle::Disc::kDumpRoot).parent_path()};
    const fs::path exe_assets = current_settings_locations().exe_dir / "assets";
    if (!exe_assets.empty()) roots.push_back(exe_assets);
    return roots;
}

}  // namespace

bool verify_enabled(bool no_verify_flag) {
    if (no_verify_flag) return false;
    const char* env = std::getenv("DCB_NO_VERIFY");
    return env == nullptr || *env == '\0' || std::strcmp(env, "0") == 0;
}

int import_command(int argc, char** argv, const char* game_id) {
    std::vector<std::string> args;
    bool force = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--force") == 0)
            force = true;
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else
            args.emplace_back(argv[i]);
    }
    if (args.empty() || args.size() > 2) {
        usage();
        return 2;
    }
    const fs::path image = args[0];
    const fs::path dest = args.size() > 1 ? fs::path(args[1]) : fs::path(hle::Disc::kDumpRoot);

    imp::Options options;
    options.overwrite = force;
    ConsoleProgress console("import");
    options.progress = [&console](const imp::Progress& p) {
        console.show(p.stage, p.done, p.total);
        return true;
    };
    try {
        const imp::Result r = imp::import_disc(image, dest, options);
        console.end();
        const char* title = imp::known_title(r.disc.serial);
        std::printf("[import] %s (%s) imported into %s: %u files, %llu MB\n", r.disc.serial.c_str(),
                    title ? title : r.disc.volume_id.c_str(), r.dir.string().c_str(), r.files,
                    static_cast<unsigned long long>(r.bytes >> 20));
        std::printf("[import] the disc image is no longer needed to play\n");
        if (r.disc.serial != game_id)
            std::printf("[import] note: this build plays %s; %s is the source of the English data (dcb --setup)\n",
                        game_id, r.disc.serial.c_str());
        return 0;
    } catch (const imp::ImportError& e) {
        console.end();
        std::fprintf(stderr, "[import] error: %s\n", e.what());
        if (e.code() == imp::ErrorCode::AlreadyExists) std::fprintf(stderr, "[import] pass --force to replace it\n");
        return 1;
    } catch (const std::exception& e) {
        console.end();
        std::fprintf(stderr, "[import] error: %s\n", e.what());
        return 1;
    }
}

bool unpack_bundled_assets(bool interactive) {
    const fs::path self = executable_path();
    if (self.empty()) return true;
    const fs::path dir = self.parent_path();
    vfs::PayloadTrailer trailer;
    switch (vfs::check_payload(self, dir, &trailer)) {
    case vfs::PayloadStatus::None: return true;  // a plain binary
    case vfs::PayloadStatus::UpToDate:
        std::printf("[dcb] bundled data: already unpacked in %s\n", (dir / "assets").string().c_str());
        return true;
    case vfs::PayloadStatus::NotOurs:
        std::printf("[dcb] bundled data: %s was not unpacked by this program; using it as it is "
                    "(remove it to unpack the bundled copy)\n",
                    (dir / "assets").string().c_str());
        return true;
    default: break;
    }

    std::printf("[dcb] unpacking the bundled data (%llu MB) into %s\n",
                static_cast<unsigned long long>(trailer.size >> 20), dir.string().c_str());
    vfs::PayloadResult result;
    int shown = -10;  // last percentage printed, in steps of 10
    const auto job = [&](const std::function<void(uint64_t, uint64_t)>& window_progress) {
        result = vfs::unpack_payload(self, dir, [&](uint64_t done, uint64_t total) {
            const int pct = total ? static_cast<int>(done * 100 / total) : 100;
            if (pct / 10 != shown / 10) {
                shown = pct;
                std::printf("[dcb] unpacking: %3d%%\n", pct);
            }
            if (window_progress) window_progress(done, total);
        });
    };
    bool ran = false;
#if defined(DCB_HAS_SDL3)
    if (interactive) ran = sdl3_progress_window("Unpacking the game data...", job);
#endif
    if (!ran) job({});

    if (result.status == vfs::PayloadStatus::Unpacked) {
        std::printf("[dcb] unpacked %zu files (%llu MB)", result.files, static_cast<unsigned long long>(result.bytes >> 20));
        if (result.kept) std::printf(", kept %zu existing player file(s)", result.kept);
        if (result.removed) std::printf(", removed %zu file(s) of an older version", result.removed);
        std::printf("\n");
        return true;
    }
    const std::string text = "The game data bundled in this program could not be unpacked:\n\n" + result.error +
                             "\n\nThe game unpacks it next to the program on its first start. Move " +
                             self.filename().string() +
                             " into a folder you can write to (with about 500 MB free) and start it again.";
    std::fprintf(stderr, "[dcb] error: %s\n", text.c_str());
#if defined(DCB_HAS_SDL3)
    if (interactive) sdl3_error_box("Cannot unpack the game data", text);
#else
    (void)interactive;
#endif
    return false;
}

SetupNeeds setup_needs(const std::string& serial, const fs::path& found, bool verify) {
    SetupNeeds needs;
    needs.serial = serial;
    needs.verify = verify;
    needs.found = found;
    // The assets root is the one holding the game data when that is an import under
    // <assets>/dump/<serial>/ (e.g. next to the executable); else assets/ of the current directory,
    // where imports go (hle::Disc::kDumpRoot) and the game looks.
    needs.assets = fs::path(hle::Disc::kDumpRoot).parent_path();
    if (!found.empty() && is_import(found) && found.parent_path().filename() == "dump")
        needs.assets = found.parent_path().parent_path();
    needs.need_jp = found.empty();

    bool present = false, unfinished = false;
    std::error_code ec;
    for (const fs::path& root : english_roots(needs.assets)) {
        if (fs::exists(root / serial / patch::kStampName, ec) && !patch::is_built(root, serial)) unfinished = true;
        if (fs::is_regular_file(root / (serial + ".pak"), ec) || fs::is_directory(root / serial / "text", ec))
            present = true;
    }
    needs.need_english = unfinished || !present;
    if (needs.need_english) {
        // Building needs both discs as imports (a .cue or the older extracted/ tree is not enough).
        needs.need_jp = needs.need_jp || !is_import(needs.jp_dump());
        needs.need_us = !is_import(needs.us_dump());
    }
    return needs;
}

Imported import_checked(const fs::path& image, const fs::path& dump_root, bool verify,
                        const imp::ProgressFn& progress) {
    Imported out;
    const imp::DiscInfo info = imp::identify(image);
    out.serial = info.serial;
    out.dir = dump_root / info.serial;
    // Keep an existing complete tree (it may hold replaced assets); replace a broken one.
    if (is_import(out.dir)) {
        out.existed = true;
        return out;
    }
    if (verify) imp::verify_dump(image, progress);
    imp::Options options;
    options.overwrite = true;
    options.progress = progress;
    out.dir = imp::import_disc(image, dump_root, options).dir;
    return out;
}

void build_english(const SetupNeeds& needs, const fs::path& fixes_dir, const patch::ProgressFn& progress) {
    // The unfinished stamp: while it is there (build_all rewrites it last), setup_needs() builds again.
    const fs::path dir = needs.assets / needs.serial;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) throw std::runtime_error("cannot create " + dir.string() + ": " + ec.message());
    {
        std::ofstream stamp(dir / patch::kStampName, std::ios::trunc);
        stamp << "unfinished\n";
        if (!stamp) throw std::runtime_error("cannot write " + (dir / patch::kStampName).string());
    }
    patch::Inputs in;
    in.jp_dump = needs.jp_dump();
    in.us_dump = needs.us_dump();
    in.fixes_dir = fixes_dir;
    in.assets = needs.assets;
    in.serial = needs.serial;
    patch::build_all(in, progress);
}

int setup_command(int argc, char** argv, const char* game_id) {
    std::vector<fs::path> images;
    fs::path fixes;
    bool no_verify = false, force = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-verify") == 0)
            no_verify = true;
        else if (std::strcmp(argv[i], "--force") == 0)
            force = true;
        else if (std::strcmp(argv[i], "--fixes") == 0 && i + 1 < argc)
            fixes = argv[++i];
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            setup_usage();
            return 0;
        } else if (argv[i][0] == '-') {
            setup_usage();
            return 2;
        } else
            images.emplace_back(argv[i]);
    }
    if (images.size() != 2) {
        setup_usage();
        return 2;
    }
    std::error_code ec;
    if (!fixes.empty() && !fs::is_directory(fixes, ec)) {
        std::fprintf(stderr, "[setup] error: --fixes %s is not a folder\n", fixes.string().c_str());
        return 2;
    }

    SetupNeeds needs;
    needs.serial = game_id;
    needs.assets = fs::path(hle::Disc::kDumpRoot).parent_path();
    needs.verify = verify_enabled(no_verify);
    needs.need_jp = needs.need_us = needs.need_english = true;
    // Never build over English data this setup did not make (a bundle's, made by the Python pipeline).
    if (!force && !fs::exists(needs.assets / game_id / patch::kStampName, ec) &&
        (fs::is_regular_file(needs.assets / (std::string(game_id) + ".pak"), ec) ||
         fs::is_directory(needs.assets / game_id / "text", ec))) {
        std::fprintf(stderr,
                     "[setup] error: %s already holds English data that this setup did not make; pass --force to "
                     "replace it\n",
                     fs::absolute(needs.assets, ec).string().c_str());
        return 1;
    }
    if (!needs.verify) std::printf("[setup] --no-verify: the dumps are not checked against redump.org\n");

    ConsoleProgress console("setup");
    try {
        // Identify both first: a wrong disc fails before the long checks.
        std::string serials[2];
        for (int i = 0; i < 2; ++i) serials[i] = imp::identify(images[i]).serial;
        const int jp = serials[0] == game_id ? 0 : serials[1] == game_id ? 1 : -1;
        const int us = serials[0] == kEnglishSerial ? 0 : serials[1] == kEnglishSerial ? 1 : -1;
        if (jp < 0 || us < 0 || jp == us) {
            std::fprintf(stderr,
                         "[setup] error: the two images are %s and %s; the setup needs %s (%s) and %s (%s)\n",
                         serials[0].c_str(), serials[1].c_str(), game_id, imp::known_title(game_id), kEnglishSerial,
                         imp::known_title(kEnglishSerial));
            return 1;
        }
        for (const int i : {jp, us}) {
            const Imported r = import_checked(images[static_cast<size_t>(i)], needs.dump_root(), needs.verify,
                                              [&console](const imp::Progress& p) {
                                                  console.show(p.stage, p.done, p.total);
                                                  return true;
                                              });
            console.end();
            std::printf("[setup] %s (%s): %s %s\n", r.serial.c_str(), imp::known_title(r.serial),
                        r.existed ? "already imported in" : needs.verify ? "verified, imported into" : "imported into",
                        r.dir.string().c_str());
        }
        std::printf("[setup] building the English data into %s%s\n", needs.assets.string().c_str(),
                    fixes.empty() ? "" : (" with the fixes in " + fixes.string()).c_str());
        build_english(needs, fixes, [&console](const patch::Progress& p) {
            console.show(p.stage, p.done, p.total);
            return true;
        });
        console.end();
        std::printf("[setup] done: start dcb to play (the disc images are no longer needed)\n");
        return 0;
    } catch (const imp::ImportError& e) {
        console.end();
        std::fprintf(stderr, "[setup] error: %s\n", e.what());
        if (e.code() == imp::ErrorCode::BadDump)
            std::fprintf(stderr, "[setup] (developers: --no-verify or DCB_NO_VERIFY=1 skips this check)\n");
        return 1;
    } catch (const std::exception& e) {
        console.end();
        std::fprintf(stderr, "[setup] error: %s\n", e.what());
        std::fprintf(stderr, "[setup] the English data is incomplete; run the setup again\n");
        return 1;
    }
}

fs::path locate_or_setup(const std::string& serial, const fs::path& hint, bool interactive, bool verify) {
    const fs::path found = hle::Disc::find(serial, hint);
    const SetupNeeds needs = setup_needs(serial, found, verify);
    if (!needs.any()) return found;
#if defined(DCB_HAS_SDL3)
    if (interactive) {
        const FirstRunResult r = sdl3_first_run(needs);
        if (r.status == FirstRunResult::Done) return r.game;
        if (r.status == FirstRunResult::Quit)
            throw std::runtime_error("the first-run setup was not finished; start the game again to continue it, or "
                                     "run  dcb --setup <jp.cue> <us.cue>");
    }
#else
    (void)interactive;
#endif
    if (found.empty()) return hle::Disc::locate(serial, hint);  // throws the instructions
    std::printf("[setup] no English data for %s next to the game: it runs in Japanese. Build it with\n"
                "[setup]     dcb --setup <jp.cue|bin> <us.cue|bin>\n",
                serial.c_str());
    return found;
}

}  // namespace platform
