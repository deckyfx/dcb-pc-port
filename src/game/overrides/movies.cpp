// Native replacement for the opening overlay's "play movie N" (config/SLPS-03101/overrides.json,
// OPENSEG 801E38A0). The original streams DIGIMON.MOV from the CD (MDEC video + XA audio) using the
// segment table at OPENSEG 801F4BB8: 0 = opening, 1 = credits, 2 = BANDAI logo. When
// movie/movie<N>.mpg exists in the asset pack or folder, it plays natively instead: full
// resolution, its own audio, any key skips; otherwise the original runs.
//
// While it plays, the override keeps yielding game frames through the game's own task sleep, so
// the rest of the game keeps its usual rhythm; the host loop (main.cpp) advances the decoder one
// game frame at a time, presents its picture instead of the game's and plays its sound instead
// of the game's sound output.

#include "movies.hpp"

#include "vfs/vfs.hpp"

#include <psx/recomp.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

extern "C" void dcb_task_sleep(PsxContext* ctx);  // tasks.cpp (80014D3C)

namespace {

constexpr uint32_t kPlayMovie = 0x801E38A0u;  // OPENSEG
constexpr int kA0 = 4, kV0 = 2, kRa = 31;

std::unique_ptr<vfs::Vfs> g_vfs;

}  // namespace

namespace dcb {

MovieHost& movie_host() {
    static MovieHost host;
    return host;
}

void attach_movies(const std::vector<std::string>& mounts) {
    g_vfs = std::make_unique<vfs::Vfs>();
    std::error_code ec;
    for (const std::string& m : mounts) {
        if (!m.empty() && std::filesystem::exists(m, ec)) g_vfs->mount(m);
    }
}

}  // namespace dcb

extern "C" {

// OPENSEG 801E38A0: play movie a0 (0 opening, 1 credits, 2 BANDAI logo) until it ends or is skipped.
void dcb_movie_play(PsxContext* ctx) {
    const uint32_t index = ctx->r[kA0];
    const std::string name = "movie/movie" + std::to_string(index) + ".mpg";
    std::vector<uint8_t> bytes;
    auto& host = dcb::movie_host();
    if (!g_vfs || !g_vfs->read(name, bytes)) return psx_call_original(ctx, kPlayMovie);  // no native file
    if (!host.player.open(std::move(bytes))) {
        std::fprintf(stderr, "[movie] %s is not a playable MPEG-1 file; using the disc\n", name.c_str());
        return psx_call_original(ctx, kPlayMovie);
    }
    std::printf("[movie] playing %s natively (%dx%d)\n", name.c_str(), host.player.width(), host.player.height());
    const uint32_t ra = ctx->r[kRa];
    host.index = static_cast<int>(index);
    host.skip = false;
    host.active = true;
    while (!host.player.ended() && !host.skip) {
        ctx->r[kA0] = 1;
        dcb_task_sleep(ctx);  // one game frame: the host loop shows the movie meanwhile
    }
    host.active = false;
    std::printf("[movie] %s %s\n", name.c_str(), host.skip ? "skipped" : "finished");
    ctx->r[kRa] = ra;
    ctx->r[kV0] = 0;
}

}  // extern "C"
