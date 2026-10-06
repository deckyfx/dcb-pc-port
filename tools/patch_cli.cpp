// dcb_patch: build the English data from two imported dumps, like the first-run setup does.
//
//   dcb_patch --jp <dump/SLPS-03101> --us <dump/SLUS-01328> --out <assets-root> [--fixes DIR]
//             [--text-only | --art-only]
//   dcb_patch --rematch <AREAnn.PAK> <C/AREAnn.PAK> <out.pak>   the gameplay mods (boss rematch,
//             arena saves; patch/mods.hpp) on one city file, as the game applies them when loading
//
// For testing the C++ port against the Python pipeline (tools/patch/compare.sh).
#include "patch/mods.hpp"
#include "patch/patch.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>

namespace {

int rematch(const char* in_path, const std::string& file, const char* out_path) {
    std::ifstream in(in_path, std::ios::binary);
    const patch::Bytes pak((std::istreambuf_iterator<char>(in)), {});
    std::string why;
    const auto out = patch::mods::patch_city_pak(pak, {patch::mods::rematches_for(file), true, true}, &why);
    if (!why.empty()) std::fprintf(stderr, "dcb_patch: %s: not applied: %s\n", in_path, why.c_str());
    if (!out) {
        std::fprintf(stderr, "dcb_patch: %s: %s\n", in_path, why.c_str());
        return 1;
    }
    std::ofstream out_file(out_path, std::ios::binary);
    out_file.write(reinterpret_cast<const char*>(out->data()), static_cast<std::streamsize>(out->size()));
    out_file.close();
    if (!out_file) {
        std::fprintf(stderr, "dcb_patch: cannot write %s\n", out_path);
        return 1;
    }
    std::printf("%s: %zu -> %zu bytes\n", out_path, pak.size(), out->size());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::strcmp(argv[1], "--rematch") == 0) return rematch(argv[2], argv[3], argv[4]);
    patch::Inputs in;
    bool text = true, art = true;
    for (int i = 1; i < argc; ++i) {
        const auto arg = [&](const char* name) { return std::strcmp(argv[i], name) == 0 && i + 1 < argc; };
        if (arg("--jp")) in.jp_dump = argv[++i];
        else if (arg("--us")) in.us_dump = argv[++i];
        else if (arg("--out")) in.assets = argv[++i];
        else if (arg("--fixes")) in.fixes_dir = argv[++i];
        else if (std::strcmp(argv[i], "--text-only") == 0) art = false;
        else if (std::strcmp(argv[i], "--art-only") == 0) text = false;
        else {
            std::fprintf(stderr, "usage: dcb_patch --jp DIR --us DIR --out ASSETS [--fixes DIR] [--text-only|--art-only]\n");
            return 2;
        }
    }
    if (in.jp_dump.empty() || in.us_dump.empty() || in.assets.empty()) {
        std::fprintf(stderr, "dcb_patch: --jp, --us and --out are required\n");
        return 2;
    }
    try {
        if (text && art) patch::build_all(in);
        else if (text) patch::build_text(in);
        else patch::build_art(in);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "dcb_patch: %s\n", e.what());
        return 1;
    }
    return 0;
}
