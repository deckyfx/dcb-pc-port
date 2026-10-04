// build_art: the US art as <assets>/<serial>.pak, and the US movie as a disc override.
//
// The pak holds only what the Python pipeline (swap_us_images.py --apply, then
// `dcb_asset_ripper pack`) changes on screen: its us/<hash>.raw files and a manifest with their
// entries. The JP PNG entries of the offline pak are left out: they are the ripped art itself,
// and without an entry the texture replacer commits the game's own upload unchanged
// (src/hle/gpu/hd_textures.cpp replace_upload), which is what an identity PNG reproduces. So no
// PNG is encoded or decoded, and the game binary needs no encoder.

#include "patch.hpp"

#include "patch/art_swap.hpp"
#include "patch/embed.hpp"
#include "vfs/hash.hpp"
#include "vfs/pak.hpp"
#include "vfs/rip.hpp"

#include <fstream>
#include <string_view>
#include <system_error>

namespace patch {

namespace {

namespace fs = std::filesystem;

constexpr const char* kStage = "US art";
constexpr const char* kMovie = "DIGIMON.MOV.raw2352";

/// The manifest hd_textures.hpp reads, with the fields swap_us_images.py writes for a US entry.
std::string manifest_json(const std::string& serial, const art::Plan& plan) {
    std::string json = "{\"version\":1,\"game\":";
    vfs::json_escape(json, serial);
    json += ",\"entries\":[";
    bool first = true;
    for (const auto* list : {&plan.images, &plan.palettes}) {
        for (const art::Replacement& r : *list) {
            if (!first) json.push_back(',');
            first = false;
            const std::string key = vfs::to_hex16(r.key);
            json += "{\"img\":\"" + key + "\",\"w\":" + std::to_string(r.w) + ",\"h\":" + std::to_string(r.h) +
                    ",\"bpp\":" + std::to_string(r.bpp) + ",\"path\":\"us/" + key + ".raw\",\"us\":";
            vfs::json_escape(json, r.us);
            json += ",\"alt\":";
            vfs::json_escape(json, r.alt);
            if (r.slot_w) json += ",\"slot_w\":" + std::to_string(r.slot_w);
            json += "}";
        }
    }
    json += "]}";
    return json;
}

/// Copy `from` to `to` through a temporary file, calling `alive` between chunks; it may throw (a
/// cancel), and then nothing is written.
void copy_file(const fs::path& from, const fs::path& to, const std::function<void()>& alive) {
    std::ifstream in(from, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + from.string());
    fs::create_directories(to.parent_path());
    const fs::path tmp = to.string() + ".tmp";
    try {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
        std::vector<char> buf(4u << 20);
        while (in) {
            alive();
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            out.write(buf.data(), in.gcount());
        }
        if (!out.flush()) throw std::runtime_error("cannot write " + tmp.string());
    } catch (...) {
        std::error_code ec;
        fs::remove(tmp, ec);
        throw;
    }
    fs::rename(tmp, to);
}

}  // namespace

void build_art(const Inputs& in, const ProgressFn& progress) {
    // Steps: art::plan()'s, then the pak, then the movie.
    uint64_t plan_total = 0;
    const auto report = [&](uint64_t done, uint64_t total) {
        if (progress && !progress({kStage, done, total})) throw Cancelled();
    };
    const auto step = [&](uint64_t done, uint64_t total) {
        plan_total = total;
        report(done, total + 2);
        return true;
    };
    const std::optional<art::Plan> plan = art::plan(in.jp_dump / "fs", in.us_dump / "fs", step);
    if (!plan) throw Cancelled();  // not reached: step throws first
    if (plan->images.empty()) throw std::runtime_error("the US art swap found nothing to replace");

    // The pak: the manifest and one .raw per replaced upload.
    report(plan_total, plan_total + 2);
    vfs::PakWriter pak;
    const std::string json = manifest_json(in.serial, *plan);
    bool ok = pak.add("assets_manifest.json", reinterpret_cast<const uint8_t*>(json.data()), json.size());
    for (const auto* list : {&plan->images, &plan->palettes})
        for (const art::Replacement& r : *list) ok = ok && pak.add("us/" + vfs::to_hex16(r.key) + ".raw", r.data);
    // The sprite sizes the fitted title art is drawn at (config/<serial>/sprites.txt).
    for (const embedded::File& f : embedded::art_config())
        if (std::string_view(f.name) == "sprites.txt") ok = ok && pak.add("sprites.txt", f.data, f.size);
    if (!ok) throw std::runtime_error("US art pak: " + pak.error());
    fs::create_directories(in.assets);
    const fs::path pak_path = in.assets / (in.serial + ".pak");
    const fs::path tmp = pak_path.string() + ".tmp";
    if (!pak.write(tmp.string())) throw std::runtime_error("cannot write " + tmp.string());
    fs::rename(tmp, pak_path);

    // The US movie (the English opening), played by the game's own STR player from the disc
    // override. Only a same-size file can stand in for the JP one (disc.cpp apply_override).
    report(plan_total + 1, plan_total + 2);
    const fs::path us_movie = in.us_dump / "fs" / kMovie, jp_movie = in.jp_dump / "fs" / kMovie;
    std::error_code ec;
    const uintmax_t us_size = fs::file_size(us_movie, ec);
    const bool same_size = !ec && us_size == fs::file_size(jp_movie, ec) && !ec;
    if (same_size) {
        copy_file(us_movie, in.assets / in.serial / "disc" / kMovie, [&] { report(plan_total + 1, plan_total + 2); });
    }
    report(plan_total + 2, plan_total + 2);
}

}  // namespace patch
