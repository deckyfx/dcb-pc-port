#pragma once
// Files of our own built into the program (the public build ships without config/): generated at
// build time from the files themselves by cmake/EmbedFiles.cmake, which stay the source of truth.

#include <cstddef>
#include <span>
#include <string_view>

namespace patch::embedded {

struct File {
    const char* name;  ///< base name ("catalog-exe.txt")
    const unsigned char* data;
    size_t size;

    std::string_view text() const { return {reinterpret_cast<const char*>(data), size}; }
};

/// config/SLPS-03101/text/catalog*.txt and en*.tsv (the text catalog), names*.tsv (the
/// character names) and lines*.tsv (reworded lines; both text.cpp), sorted by name.
std::span<const File> text_config();
/// config/SLPS-03101/sprites.txt (the sprite sizes the US title art is drawn at).
std::span<const File> art_config();

}  // namespace patch::embedded
