#include "first_run.hpp"

#include "cdrom/disc.hpp"
#include "cdrom/importer.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace platform {

namespace fs = std::filesystem;

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: dcb --import <disc.cue|disc.bin> [dest-dir] [--force]\n"
                 "  Imports a dump of your own disc (raw .cue/.bin, 2352 bytes per sector) into\n"
                 "  <dest-dir>/<serial>/ (default: extracted/ in the current directory). The game then\n"
                 "  runs from those files; the disc image is no longer needed.\n"
                 "  --force  replace an existing <dest-dir>/<serial>/\n");
}

}  // namespace

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
    const fs::path dest = args.size() > 1 ? fs::path(args[1]) : fs::path("extracted");

    hle::import::Options options;
    options.overwrite = force;
    int shown = -1;
    options.progress = [&shown](const hle::import::Progress& p) {
        const int pct = p.total ? static_cast<int>(p.done * 100 / p.total) : 0;
        if (pct != shown) {
            shown = pct;
            std::fprintf(stderr, "\r[import] %3d%%  %-18s", pct, p.stage);
            std::fflush(stderr);
        }
        return true;
    };
    try {
        const hle::import::Result r = hle::import::import_disc(image, dest, options);
        std::fprintf(stderr, "\n");
        const char* title = hle::import::known_title(r.disc.serial);
        std::printf("[import] %s (%s) imported into %s: %u files, %llu MB\n", r.disc.serial.c_str(),
                    title ? title : r.disc.volume_id.c_str(), r.dir.string().c_str(), r.files,
                    static_cast<unsigned long long>(r.bytes >> 20));
        std::printf("[import] the disc image is no longer needed to play\n");
        if (r.disc.serial != game_id)
            std::printf("[import] note: this build plays %s; %s is kept for the planned Japanese-code + English-assets "
                        "build\n",
                        game_id, r.disc.serial.c_str());
        return 0;
    } catch (const hle::import::ImportError& e) {
        std::fprintf(stderr, "\n[import] error: %s\n", e.what());
        if (e.code() == hle::import::ErrorCode::AlreadyExists)
            std::fprintf(stderr, "[import] pass --force to replace it\n");
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\n[import] error: %s\n", e.what());
        return 1;
    }
}

fs::path locate_or_import(const std::string& serial, const fs::path& hint, bool interactive) {
    if (fs::path found = hle::Disc::find(serial, hint); !found.empty()) return found;
#if defined(DCB_HAS_SDL3)
    if (interactive) {
        if (fs::path imported = sdl3_first_run(serial, "extracted"); !imported.empty()) return imported;
    }
#else
    (void)interactive;
#endif
    return hle::Disc::locate(serial, hint);  // throws the instructions
}

}  // namespace platform
