#pragma once
// Shared with other overrides: the text catalog of text.cpp.

#include <psx/recomp.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dcb {

/// The catalog translation of a whole game string (loads the English assets on first use).
/// False when there is none, or no English assets.
bool text_translate(PsxContext& ctx, const std::string& in, std::string& out);

/// [text] names (settings.ini): true ("jp", the default) shows the Japanese character names
/// (Iori, Miyako...) instead of the US ones (config/<serial>/text/names-jp.tsv).
void text_set_jp_names(bool on);
/// The character names in `s` as that setting asks (whole words). For lines kept host-side and
/// typed out (city_text.cpp, event_text.cpp): swap before the player's own name goes in.
void text_swap_names(std::string& s);
/// A name as the game data holds it (a deck name cut to 11 letters and a tag byte when the US
/// one is longer: en_names.txt) in full; other names as they are.
std::string text_full_name(const std::string& s);
/// A US line the port rewords (config/<serial>/text/lines-en.tsv), given the line before it (some
/// rewordings apply only after a given line). For lines kept host-side (city_text.cpp).
void text_reword_line(std::string& line, const std::string& previous);

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

/// Draws a VRAM image (e.g. a portrait the port uploaded) as one sprite in OT entry `ot`: (x, y)
/// on screen, w x h pixels from texel (u, v) of texture page `tpage` (the GP0 E1h bits: page x/64,
/// page y/256 << 4, colour depth << 7) with palette `clut` ((y << 6) | (x >> 4)). Through the text
/// primitive pool, like the text; nothing when it is full.
void text_draw_image(PsxContext& ctx, int x, int y, int w, int h, int u, int v, uint16_t tpage, uint16_t clut, int ot);

/// Draws `s` exactly as typed, one character at a time: no catalog, long names or deck label,
/// and letters are letters (never JP codes). ASCII takes the US font, Shift-JIS the JP original
/// (draw arguments as 8002AE00: rgb is a guest pointer). `x_of` (optional) receives each
/// character's x, then the end x. For a name being typed (name_entry.cpp).
void text_draw_verbatim(PsxContext& ctx, int x, int y, int clut, int prop, uint32_t rgb, int ot,
                        const std::string& s, std::vector<int>* x_of);

}  // namespace dcb
