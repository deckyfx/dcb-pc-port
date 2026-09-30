// The ASCII font and the card / deck text: ports of build_font, graft_cdd and graft_dek in
// tools/text/en_text.py (docs/re/text-engine.md 4.2, HYBRID section 6).

#include "text_internal.hpp"

#include <algorithm>
#include <cstdio>

namespace patch::text {

namespace {

// en_font.bin: US ASCII font rows (TIM pixel rows 48..223 of B:SYSTEM.TIM) + the 96-byte width
// table of the US draw routine (EXE VA 0x8006DF9C: lui 0x8007; addiu -8292).
constexpr uint16_t kFontFirstRow = 48, kFontLastRow = 223;
constexpr uint32_t kWidthVa = 0x8006DF9C, kTextAddr = 0x80010000, kWidthCount = 96;

View slice(View b, size_t a, size_t e) {
    a = std::min(a, b.size());
    e = std::clamp(e, a, b.size());
    return b.subspan(a, e - a);
}

/// en_text.us_line: up to the first byte >= 0x80 (leftover data after a missing NUL).
View us_line(View t) {
    const auto it = std::find_if(t.begin(), t.end(), [](uint8_t c) { return c >= 0x80; });
    return t.first(static_cast<size_t>(it - t.begin()));
}

/// Up to the first NUL.
View until_nul(View t) {
    const auto it = std::find(t.begin(), t.end(), uint8_t{0});
    return t.first(static_cast<size_t>(it - t.begin()));
}

/// Write `src` (at most `max` bytes) into out[at, at + slot), NUL-padded.
void put(Bytes& out, size_t at, View src, size_t max, size_t slot) {
    const size_t n = std::min(src.size(), max);
    std::copy(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(n), out.begin() + static_cast<std::ptrdiff_t>(at));
    std::fill(out.begin() + static_cast<std::ptrdiff_t>(at + n), out.begin() + static_cast<std::ptrdiff_t>(at + slot), uint8_t{0});
}

void copy_field(Bytes& out, size_t jo, View us, size_t uo, size_t len) {
    const View f = slice(us, uo, uo + len);
    std::copy(f.begin(), f.end(), out.begin() + static_cast<std::ptrdiff_t>(jo));
}

/// Four effect-text lines: US 21-byte slots at us[uo] -> JP 19-byte slots at out[jo].
void effect_lines(Bytes& out, size_t jo, View us, size_t uo, const char* what, int index,
                  std::vector<std::string>& report) {
    for (int li = 0; li < 4; ++li) {
        const size_t u = uo + static_cast<size_t>(li) * 21;
        const View src = us_line(until_nul(slice(us, u, u + 21)));
        if (src.size() + 1 > 19) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%s %d line %d (%zu chars): ", what, index, li, src.size());
            report.push_back(buf + py_repr(src));
        }
        put(out, jo + static_cast<size_t>(li) * 19, src, 18, 19);
    }
}

}  // namespace

std::string py_repr(View b) {
    const bool single = std::find(b.begin(), b.end(), uint8_t{'\''}) != b.end();
    const bool dbl = std::find(b.begin(), b.end(), uint8_t{'"'}) != b.end();
    const char quote = single && !dbl ? '"' : '\'';
    std::string s = "b";
    s += quote;
    for (uint8_t c : b) {
        if (c == static_cast<uint8_t>(quote) || c == '\\') {
            s += '\\';
            s += static_cast<char>(c);
        } else if (c == '\t') {
            s += "\\t";
        } else if (c == '\n') {
            s += "\\n";
        } else if (c == '\r') {
            s += "\\r";
        } else if (c < 0x20 || c >= 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            s += buf;
        } else {
            s += static_cast<char>(c);
        }
    }
    s += quote;
    return s;
}

Bytes build_font(View us_tim, View us_exe) {
    const std::vector<TimBlock> blocks = tim_blocks(us_tim);
    // The image block: the larger payload (the CLUT block is 16x16); the first one on a tie.
    const TimBlock* img = &blocks[0];
    for (const TimBlock& b : blocks)
        if (b.payload.size() > img->payload.size()) img = &b;
    if (img->w != 64 || img->h != 256) throw std::runtime_error("unexpected SYSTEM.TIM image size");
    const View rows = slice(img->payload, size_t{kFontFirstRow} * 128, (size_t{kFontLastRow} + 1) * 128);
    const View text = slice(us_exe, 0x800, us_exe.size());
    const View widths = slice(text, kWidthVa - kTextAddr, kWidthVa - kTextAddr + kWidthCount);
    if (widths.size() != kWidthCount) throw std::runtime_error("width table runs past text end");
    Bytes out;
    wr16(out, kFontFirstRow);
    wr16(out, kFontLastRow);
    out.insert(out.end(), rows.begin(), rows.end());
    out.insert(out.end(), widths.begin(), widths.end());
    return out;
}

Bytes graft_cdd(View jp, View us, std::vector<std::string>& report) {
    if (jp.size() < 8 || us.size() < 8 || str(jp.first(4)) != "ADCD" || str(us.first(4)) != "0ACD")
        throw std::runtime_error("CARD2.CDD: bad magic");
    const size_t n_dig = rd16(jp, 4), n_item = jp[6], n_opt = jp[7];
    if (n_dig != 191 || n_item != 102 || n_opt != 8) throw std::runtime_error("CARD2.CDD: unexpected record counts");
    if (jp.size() < 8 + n_dig * 0x134 + n_item * 0xDA + n_opt * 0x68 || us.size() < 8 + n_dig * 0x13C + n_item * 0xE2 + n_opt * 0x70)
        throw std::runtime_error("CARD2.CDD: records past the end");
    Bytes out(jp.begin(), jp.end());
    // Digimon: stride JP 0x134 / US 0x13C; name[21] at +3, attack names[22] at +26/+42/+5E,
    // effect text at +E7.
    for (size_t i = 0; i < n_dig; ++i) {
        const size_t jo = 8 + i * 0x134, uo = 8 + i * 0x13C;
        copy_field(out, jo + 3, us, uo + 3, 21);
        for (size_t ao : {0x26, 0x42, 0x5E}) copy_field(out, jo + ao, us, uo + ao, 22);
        effect_lines(out, jo + 0xE7, us, uo + 0xE7, "digimon", static_cast<int>(i), report);
    }
    // Items: stride JP 0xDA / US 0xE2, effects at +8D. Options: JP 0x68 / US 0x70, effects at +1B.
    size_t jbase = 8 + n_dig * 0x134, ubase = 8 + n_dig * 0x13C;
    for (size_t i = 0; i < n_item; ++i) {
        const size_t jo = jbase + i * 0xDA, uo = ubase + i * 0xE2;
        copy_field(out, jo + 3, us, uo + 3, 21);
        effect_lines(out, jo + 0x8D, us, uo + 0x8D, "item", static_cast<int>(i), report);
    }
    jbase += n_item * 0xDA;
    ubase += n_item * 0xE2;
    for (size_t i = 0; i < n_opt; ++i) {
        const size_t jo = jbase + i * 0x68, uo = ubase + i * 0x70;
        copy_field(out, jo + 3, us, uo + 3, 21);
        effect_lines(out, jo + 0x1B, us, uo + 0x1B, "option", static_cast<int>(i), report);
    }
    return out;
}

Bytes graft_dek(View jp, View us, std::vector<std::string>& report, std::vector<LongName>& long_names) {
    // 159 records, JP stride 104 / US 110: card list +0..+59 (identical), deck name JP 13 B /
    // US 19 B at +60, owner 21 B (JP +73, US +79).
    if (jp.size() < 4 || us.size() < 4 || str(jp.first(4)) != "20KD" || str(us.first(4)) != "30KD")
        throw std::runtime_error("DECK2.DEK: bad magic");
    if (jp.size() != 8 + 159 * 104 || us.size() != 8 + 159 * 110) throw std::runtime_error("DECK2.DEK: bad size");
    Bytes out(jp.begin(), jp.end());
    // Keyed by std::string (not Bytes): GCC 13 warns falsely on std::less<std::vector<uint8_t>>.
    std::map<std::string, int> tags;    // prefix -> tags used
    std::map<std::string, int> tag_of;  // full name -> its tag
    for (size_t i = 0; i < 159; ++i) {
        const size_t jo = 8 + i * 104, uo = 8 + i * 110;
        if (!equal(View(out).subspan(jo, 60), us.subspan(uo, 60)))
            throw std::runtime_error("DECK2.DEK: deck " + std::to_string(i) + ": card list differs");
        View src = until_nul(us.subspan(uo + 60, 19));
        if (src.size() + 1 > 13) {
            // Too long for the slot: the first 11 letters and a tag byte (1, 2... per shared
            // prefix) key the full name in en_names.txt; the renderer draws the full name.
            const Bytes prefix(src.begin(), src.begin() + 11), full(src.begin(), src.end());
            const std::string name(str(full));
            if (!tag_of.count(name)) {
                tag_of[name] = ++tags[std::string(str(prefix))];
                long_names.push_back({prefix, tag_of[name], full});
            }
            const int tag = tag_of[name];
            if (tag > 9) throw std::runtime_error("DECK2.DEK: more than 9 long names share a prefix");
            Bytes key = prefix;
            key.push_back(static_cast<uint8_t>(tag));
            put(out, jo + 60, key, 13, 13);
        } else {
            put(out, jo + 60, src, 13, 13);
        }
        src = until_nul(us.subspan(uo + 79, 21));
        if (src.size() + 1 > 21) {
            char buf[48];
            std::snprintf(buf, sizeof buf, "deck %zu owner (%zu chars): ", i, src.size());
            report.push_back(buf + py_repr(src));
        }
        put(out, jo + 73, src, 20, 21);
    }
    return out;
}

}  // namespace patch::text
