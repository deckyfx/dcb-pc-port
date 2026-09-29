#pragma once
// Shared with other overrides: the text catalog of text.cpp.

#include <psx/recomp.h>

#include <string>

namespace dcb {

/// The catalog translation of a whole game string (loads the English assets on first use).
/// False when there is none, or no English assets.
bool text_translate(PsxContext& ctx, const std::string& in, std::string& out);

}  // namespace dcb
