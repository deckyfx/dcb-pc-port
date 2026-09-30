// dcb_patch: build the English data from two imported dumps, like the first-run setup does.
//
//   dcb_patch --jp <dump/SLPS-03101> --us <dump/SLUS-01328> --out <assets-root> [--fixes DIR]
//             [--text-only | --art-only]
//
// For testing the C++ port against the Python pipeline (tools/patch/compare.sh).
#include "patch/patch.hpp"

#include <cstdio>
#include <cstring>
#include <exception>

int main(int argc, char** argv) {
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
