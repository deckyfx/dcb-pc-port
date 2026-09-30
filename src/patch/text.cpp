// patch::build_text: the loose English files for the JP build, from the player's two dumps. A
// C++ port of tools/text/en_text.py (and the modules it imports, see text_internal.hpp); the
// output matches the Python pipeline byte for byte (tools/patch/compare_text.sh checks it):
//
//   en_font.bin            US ASCII font rows + width table
//   en_bigfont.bin         VS-screen big-name font; files/B/MATCH/NNN.ARC the name pictures
//   files/B/CARD2.CDD      card names, attacks, effect text; files/B/DECK2.DEK deck / owner names
//   en_names.txt           deck names too long for their slot; en_text_report.txt overlong text
//   files/P/<SEG>.BIN      overlay data changes of the community fixes (optional input)
//   files/C/AREAnn.PAK     city scripts; files/B/BETA.MSD, files/C/EVENT/UNIT0n.MSD scenario scripts
//   text/source.tsv, text/en.tsv   the text catalog (embedded config/SLPS-03101/text)

#include "embed.hpp"
#include "patch.hpp"
#include "text_internal.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>

namespace patch {

namespace {

namespace fs = std::filesystem;
using namespace patch::text;

void write_file(const fs::path& path, View data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out) throw std::runtime_error("cannot write " + path.string());
}

/// The boot executable's name on the disc: the BOOT line of fs/SYSTEM.CNF ("cdrom:\SLUS_013.28;1").
std::string boot_name(const fs::path& dump) {
    std::ifstream in(dump / "fs" / "SYSTEM.CNF", std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line.compare(0, 4, "BOOT") != 0) continue;
        std::string v = line.substr(eq + 1);
        if (const size_t colon = v.find(':'); colon != std::string::npos) v = v.substr(colon + 1);
        v.erase(0, v.find_first_not_of(" \t\\"));
        v = v.substr(0, v.find_first_of(";\r\n \t"));
        if (!v.empty()) return v;
    }
    return {};
}

/// The boot executable of a dump: fs/<BOOT name> (native imports have no exe/), else exe/boot.exe
/// (the older Python extraction).
Bytes read_boot(const fs::path& dump, const std::string& name) {
    std::error_code ec;
    if (!name.empty() && fs::is_regular_file(dump / "fs" / name, ec)) return read_file((dump / "fs" / name).string());
    return read_file((dump / "exe" / "boot.exe").string());
}

/// Every *.xdelta in `dir`, by name (the Python sorted glob).
std::vector<std::pair<std::string, Bytes>> read_patches(const fs::path& dir) {
    std::vector<std::pair<std::string, Bytes>> out;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return out;
    std::vector<fs::path> paths;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() >= 7 && name.compare(name.size() - 7, 7, ".xdelta") == 0) paths.push_back(e.path());
    }
    std::sort(paths.begin(), paths.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename().string() < b.filename().string();
    });
    for (const fs::path& p : paths) out.emplace_back(p.filename().string(), read_file(p.string()));
    return out;
}

/// Delete out/<dir>/*<ext> not in `keep` (a fix or archive since dropped).
void remove_stale(const fs::path& dir, const char* ext, const std::set<std::string>& keep) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    std::vector<fs::path> stale;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ext && !keep.count(e.path().filename().string())) stale.push_back(e.path());
    for (const fs::path& p : stale) fs::remove(p, ec);
}

void warn(const std::string& line) { std::fprintf(stderr, "patch: %s\n", line.c_str()); }

}  // namespace

void build_text(const Inputs& in, const ProgressFn& progress) {
    constexpr uint64_t kSteps = 7;
    uint64_t step = 0;
    const auto next = [&] {
        if (progress && !progress(Progress{"English text", step, kSteps})) throw Cancelled();
        ++step;
    };
    const fs::path out = in.assets / in.serial;
    const fs::path jp_fs = in.jp_dump / "fs", us_fs = in.us_dump / "fs";

    // 0. Community fixes: the reference files they correct (text_fixes.cpp).
    next();
    fs::create_directories(out / "files" / "B");
    Fixed fixed;
    if (auto patches = read_patches(in.fixes_dir); !patches.empty()) {
        try {
            std::vector<DiscFile> disc_files;
            const Bytes image = rebuild_image(in.us_dump, disc_files);
            fixed = apply_fixes(image, disc_files, patches);
        } catch (const std::exception& e) {
            fixed.problems.push_back(std::string("the fixes need the reference disc: ") + e.what());
        }
        for (const std::string& p : fixed.problems) warn("fix: " + p);
    }
    const auto us_read = [&](const std::string& name) {
        const auto it = fixed.files.find(name);
        return it != fixed.files.end() && !it->second.empty() ? it->second : read_file((us_fs / name).string());
    };

    const Bytes jp_b = read_file((jp_fs / "B.DRV").string()), us_b = us_read("B.DRV");
    const Drv jp_bd(jp_b), us_bd(us_b);

    // 1. Font rows + width table; the VS-screen big names.
    next();
    const std::string us_boot = boot_name(in.us_dump);
    const auto fixed_exe = fixed.files.find(us_boot);
    const Bytes us_exe = fixed_exe != fixed.files.end() && !fixed_exe->second.empty() ? fixed_exe->second
                                                                                     : read_boot(in.us_dump, us_boot);
    write_file(out / "en_font.bin", build_font(us_bd.file("SYSTEM.TIM"), us_exe));
    {
        const auto us_font = us_bd.find_last("FONT.ARC");
        if (!us_font) throw std::runtime_error("FONT.ARC not in the US B.DRV");
        write_file(out / "en_bigfont.bin", build_bigfont(*us_font));
        // Match archives in name order (bigfont.write_assets iterates a sorted dict).
        std::set<std::string> names;
        for (const Drv::Entry& e : jp_bd.entries())
            if (e.path.rfind("MATCH/", 0) == 0 && e.path.size() >= 4 && e.path.compare(e.path.size() - 4, 4, ".ARC") == 0)
                names.insert(e.path);
        const fs::path mdir = out / "files" / "B" / "MATCH";
        std::set<std::string> written;
        for (const std::string& name : names) {
            const auto us_arc = us_bd.find_last(name);
            if (!us_arc) continue;
            const auto arc = graft_match_name(*jp_bd.find_last(name), *us_arc);
            if (!arc) continue;
            fs::create_directories(mdir);
            const std::string base = name.substr(6);
            write_file(mdir / base, *arc);
            written.insert(base);
        }
        remove_stale(mdir, ".ARC", written);
    }

    // 2. Card + deck text.
    next();
    std::vector<std::string> report;
    const View jp_cdd = jp_bd.file("CARD2.CDD"), us_cdd = us_bd.file("CARD2.CDD");
    write_file(out / "files" / "B" / "CARD2.CDD", graft_cdd(jp_cdd, us_cdd, report));
    std::vector<std::string> diffs;  // balance bytes the JP keeps (for the report)
    for (size_t c : {5, 13, 128}) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "card %zu +0x8E: JP 0x%02x US 0x%02x (kept JP)", c,
                      jp_cdd[8 + c * 0x134 + 0x8E], us_cdd[8 + c * 0x13C + 0x8E]);
        diffs.push_back(buf);
    }
    std::vector<LongName> long_names;
    write_file(out / "files" / "B" / "DECK2.DEK",
               graft_dek(jp_bd.file("DECK2.DEK"), us_bd.file("DECK2.DEK"), report, long_names));
    Bytes names;
    for (const LongName& n : long_names) {
        names.insert(names.end(), n.prefix.begin(), n.prefix.end());
        names.push_back('\t');
        const std::string tag = std::to_string(n.tag);
        names.insert(names.end(), tag.begin(), tag.end());
        names.push_back('\t');
        names.insert(names.end(), n.full.begin(), n.full.end());
        names.push_back('\n');
    }
    write_file(out / "en_names.txt", names);
    std::string rep = "Overlong strings (US text + NUL > JP slot; shorten by hand):\n";
    for (const std::string& l : report) rep += "  " + l + "\n";
    rep += "\nBalance bytes kept JP:\n";
    for (const std::string& l : diffs) rep += "  " + l + "\n";
    rep += "\n";
    write_file(out / "en_text_report.txt", view(rep));

    // 2b. Data fixes in the overlays, carried into the SLPS overlays (files/P/<NAME>.BIN).
    next();
    const Bytes jp_p = read_file((jp_fs / "P.DRV").string());
    const Drv jp_pd(jp_p);
    {
        const fs::path p_dir = out / "files" / "P";
        std::set<std::string> written;
        if (const auto it = fixed.files.find("P.DRV"); it != fixed.files.end()) {
            const Bytes us_before = read_file((us_fs / "P.DRV").string());
            const Drv before_d(us_before), after_d(it->second);
            for (const Drv::Entry& e : jp_pd.entries()) {
                const View before = before_d.file(e.path), after = after_d.file(e.path);
                if (equal(before, after)) continue;
                const View target = jp_pd.file(e.path);
                const Bytes ported = port_fix(before, after, target);
                if (equal(ported, target)) continue;
                fs::create_directories(p_dir);
                write_file(p_dir / e.path, ported);
                written.insert(e.path);
            }
        }
        remove_stale(p_dir, ".BIN", written);
    }

    // 3. City scripts: C:\AREAnn.PAK with the US script chunk.
    next();
    const Bytes jp_c = read_file((jp_fs / "C.DRV").string()), us_c = us_read("C.DRV");
    const Drv jp_cd(jp_c), us_cd(us_c);
    fs::create_directories(out / "files" / "C");
    for (int n = 0; n < 12; ++n) {
        char name[16];
        std::snprintf(name, sizeof name, "AREA%02d.PAK", n);
        std::string why;
        const auto pak = graft_city_script(jp_cd.file(name), us_cd.file(name), &why);
        if (!pak) {
            warn(std::string("city ") + name + ": kept JP (" + why + ")");
            continue;
        }
        write_file(out / "files" / "C" / name, *pak);
    }

    // 3b. Tutorial + Fusion Shop scripts.
    next();
    for (const ScriptSpec& spec : scenario_scripts()) {
        const bool b = spec.drive[0] == 'B';
        const fs::path dst = out / "files" / spec.drive / spec.path;
        std::string why;
        const auto script = graft_script((b ? jp_bd : jp_cd).file(spec.path), (b ? us_bd : us_cd).file(spec.path), spec, &why);
        std::error_code ec;
        if (!script) {
            warn(std::string("script ") + spec.drive + ":" + spec.path + ": kept JP (" + why + ")");
            fs::remove(dst, ec);
            continue;
        }
        fs::create_directories(dst.parent_path());
        write_file(dst, *script);
    }

    // 4. Text catalog: source.tsv + en.tsv.
    next();
    const Bytes us_p = us_read("P.DRV");
    const Drv us_pd(us_p);
    const Bytes jp_exe = read_boot(in.jp_dump, boot_name(in.jp_dump));
    const auto loader = [](const Bytes& exe, const Drv& p_drv) {
        return [&exe, &p_drv](const std::string& name) -> View {
            return name == "EXE" ? View(exe) : p_drv.file(name + ".BIN");
        };
    };
    std::map<std::string, Bytes> own;
    std::string catalog_text;
    for (const embedded::File& f : embedded::text_config()) {  // sorted by name
        const std::string_view name = f.name;
        if (name.rfind("en", 0) == 0 && name.size() >= 6 && name.substr(name.size() - 4) == ".tsv")
            read_own(view(f.text()), own);
        else if (name.rfind("catalog", 0) == 0 && name.size() >= 11 && name.substr(name.size() - 4) == ".txt")
            catalog_text += std::string(f.text()) + "\n";
    }
    Rows source, en;
    std::vector<std::string> problems;
    build_catalog(catalog_text, loader(jp_exe, jp_pd), loader(us_exe, us_pd), own, source, en, problems);
    std::set<std::string> seen;
    for (const auto& row : source)
        if (!seen.insert(row.first).second) problems.push_back(row.first + ": listed twice");
    fs::create_directories(out / "text");
    write_file(out / "text" / "source.tsv", write_rows(source));
    write_file(out / "text" / "en.tsv", write_rows(en, &own));
    for (const std::string& p : problems) warn("catalog: " + p);

    if (progress) progress(Progress{"English text", kSteps, kSteps});
}

}  // namespace patch
