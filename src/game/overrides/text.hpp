#pragma once
// Shared with other overrides: the text catalog of text.cpp.

#include <psx/recomp.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace dcb {

/// The catalog translation of a whole game string (loads the English assets on first use).
/// False when there is none, or no English assets.
bool text_translate(PsxContext& ctx, const std::string& in, std::string& out);

/// A string address the text engine reads somewhere else: a draw (8002AE00) of the string at
/// `from` draws the one at `to`.
struct TextAlias {
    uint32_t from;
    uint32_t to;
};

/// Install `count` aliases (nullptr / 0 clears them). For labels whose addresses are code
/// immediates (the name entry's tab list, name_entry.cpp): set just around the game call that
/// draws them, so they never touch another screen that reuses the overlay window.
void text_set_aliases(const TextAlias* list, size_t count);

}  // namespace dcb
