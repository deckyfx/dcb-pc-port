// Scenario scripts in English: the city scripts (en_text.graft_city_script) and the tutorial /
// Fusion Shop scripts (tools/text/scripts.py). A US script replaces the JP one only when both
// are the same program apart from text (msd_same_program); see docs/re/text-engine.md 7.11.

#include "text_internal.hpp"

#include <algorithm>
#include <map>

namespace patch::text {

namespace {

bool contains(View hay, std::string_view needle) {
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}

/// A "*b0".."*b2" button icon at `i`.
bool icon_at(View t, size_t i) {
    return i + 2 < t.size() && t[i] == '*' && t[i + 1] == 'b' && t[i + 2] >= '0' && t[i + 2] <= '2';
}

/// `text` with every button icon replaced by map[digit] (left to right, like re.sub).
Bytes remap_icons(View text, const char* map /* new digit for '0','1','2' */) {
    Bytes out(text.begin(), text.end());
    for (size_t i = 0; i < out.size();) {
        if (icon_at(out, i)) {
            out[i + 2] = static_cast<uint8_t>(map[out[i + 2] - '0']);
            i += 3;
        } else {
            ++i;
        }
    }
    return out;
}

// Button icons the US city scripts name differently: `*b1` is only ever the map button, which the
// JP scripts draw with icon b2 (en_text.CITY_BUTTONS).
Bytes remap_city_buttons(View script) {
    Bytes out(script.begin(), script.end());
    for (const MsdRecord& r : msd_walk(script)) {
        if (r.op != kMsdText || !r.has_text) continue;
        Bytes t(r.text.begin(), r.text.end());
        for (size_t i = 0; i + 3 <= t.size(); ++i)  // bytes.replace(b"*b1", b"*b2")
            if (t[i] == '*' && t[i + 1] == 'b' && t[i + 2] == '1') {
                t[i + 2] = '2';
                i += 2;
            }
        std::copy(t.begin(), t.end(), out.begin() + static_cast<std::ptrdiff_t>(r.offset + 6));
    }
    return out;
}

// Tutorial (KAWSEG host): icons b0 b1 b2 are circle, triangle, cross. The US confirms with cross,
// redraws with triangle and skips with circle; this build keeps the JP controls
// (scripts.TUTORIAL_BUTTONS: *b2 -> *b0, *b1 -> *b2, *b0 -> *b1).
constexpr char kTutorialButtons[] = {'1', '2', '0'};

/// The b0-b2 icon digits of a JP line (bare "bN" codes; Shift-JIS pairs skipped).
std::string jp_icons(View t) {
    std::string out;
    for (size_t i = 0; i < t.size();) {
        const uint8_t c = t[i];
        if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) {
            i += 2;
        } else if (c == 'b' && i + 1 < t.size() && t[i + 1] >= '0' && t[i + 1] <= '2') {
            out += static_cast<char>(t[i + 1]);
            i += 2;
        } else {
            ++i;
        }
    }
    return out;
}

/// The "*bN" icon digits of a US line.
std::string us_icons(View t) {
    std::string out;
    for (size_t i = 0; i < t.size();) {
        if (icon_at(t, i)) {
            out += static_cast<char>(t[i + 2]);
            i += 3;
        } else {
            ++i;
        }
    }
    return out;
}

/// Text records between consecutive program (non text-side) records.
std::vector<std::vector<MsdRecord>> segments(View script, const ShowText& show_text) {
    std::vector<std::vector<MsdRecord>> segs(1);
    for (const MsdRecord& r : msd_walk(script)) {
        if (msd_is_text_side(r, show_text)) {
            if (r.op == kMsdText) segs.back().push_back(r);
        } else {
            segs.emplace_back();
        }
    }
    return segs;
}

/// scripts.remap_tutorial_buttons: new text (by record offset in `us`) for the prompts that name
/// confirm / redraw / skip. Per stretch of text between the same two program records: when the
/// US names the same icons as the JP they stay; otherwise every line not about attacks is remapped.
std::map<size_t, Bytes> remap_tutorial_buttons(View jp, View us, const ShowText& show_text) {
    std::map<size_t, Bytes> changes;
    const auto a = segments(jp, show_text), b = segments(us, show_text);
    for (size_t n = 0; n < std::min(a.size(), b.size()); ++n) {
        std::string ji, ui;
        for (const MsdRecord& r : a[n]) ji += jp_icons(r.text);
        for (const MsdRecord& r : b[n]) ui += us_icons(r.text);
        if (ji == ui) continue;
        for (const MsdRecord& r : b[n]) {
            if (!r.has_text || contains(r.text, "Attack") || us_icons(r.text).empty()) continue;
            changes[r.offset] = remap_icons(r.text, kTutorialButtons);
        }
    }
    return changes;
}

}  // namespace

std::optional<Bytes> graft_city_script(View jp_pak, View us_pak, std::string* why) {
    std::vector<PakChunk> chunks = read_pak(jp_pak);
    std::map<uint16_t, Bytes> us_scripts;
    for (PakChunk& c : read_pak(us_pak))
        if (c.kind == 2) us_scripts[c.id] = std::move(c.data);
    for (PakChunk& c : chunks) {
        if (c.kind != 2) continue;
        const auto it = us_scripts.find(c.id);
        if (it == us_scripts.end()) {
            if (why) *why = "no US script chunk";
            return std::nullopt;
        }
        if (!msd_same_program(c.data, it->second, city_show_text(), {}, why)) return std::nullopt;
        c.data = remap_city_buttons(it->second);
    }
    return write_pak(chunks);
}

const std::vector<ScriptSpec>& scenario_scripts() {
    // B:\BETA.MSD: tutorial, host KAWSEG: 0x0A cmd 0 shows text; registers 4-7 hold the pad
    // buttons pressed. C:\EVENT\UNIT0n.MSD: Fusion Shop scenes, host EVOSEG: 0x0A cmd 0 adds a
    // line, cmd 11 waits for circle and clears the page.
    static const std::vector<ScriptSpec> k{
        {"B", "BETA.MSD", {{0x0A, 0}}, {4, 5, 6, 7}},
        {"C", "EVENT/UNIT00.MSD", {{0x0A, 0}, {0x0A, 0x0B}}, {}},
        {"C", "EVENT/UNIT01.MSD", {{0x0A, 0}, {0x0A, 0x0B}}, {}},
        {"C", "EVENT/UNIT02.MSD", {{0x0A, 0}, {0x0A, 0x0B}}, {}},
    };
    return k;
}

std::optional<Bytes> graft_script(View jp, View us, const ScriptSpec& spec, std::string* why) {
    if (!msd_same_program(jp, us, spec.show_text, spec.button_regs, why)) return std::nullopt;
    Bytes out(us.begin(), us.end());
    // The JP button tests (same places, same jumps: checked by msd_same_program).
    const auto a = msd_skeleton(jp, spec.show_text), b = msd_skeleton(us, spec.show_text);
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        if (!equal(a[i].raw, b[i].raw) && msd_is_button_test(a[i], spec.button_regs))
            std::copy(a[i].raw.begin(), a[i].raw.end(), out.begin() + static_cast<std::ptrdiff_t>(b[i].offset));
    std::map<size_t, Bytes> changes;
    if (!spec.button_regs.empty()) changes = remap_tutorial_buttons(jp, us, spec.show_text);
    // The US Fusion Shop text writes a double quote as the six characters \0x22.
    static constexpr std::string_view kQuote = "\\0x22";
    for (const MsdRecord& r : msd_walk(us)) {
        if (r.op != kMsdText || !r.has_text) continue;
        const auto ch = changes.find(r.offset);
        Bytes t = ch != changes.end() ? ch->second : Bytes(r.text.begin(), r.text.end());
        for (auto it = std::search(t.begin(), t.end(), kQuote.begin(), kQuote.end()); it != t.end();
             it = std::search(it, t.end(), kQuote.begin(), kQuote.end())) {
            *it = '"';
            it = t.erase(it + 1, it + static_cast<std::ptrdiff_t>(kQuote.size()));
        }
        if (equal(t, r.text)) continue;
        // In place: the record keeps its length (a shorter string is NUL-padded).
        const size_t length = rd16(r.raw, 4);
        if (t.size() > length) throw std::runtime_error("a text record cannot grow in place");
        t.resize(length, 0);
        std::copy(t.begin(), t.end(), out.begin() + static_cast<std::ptrdiff_t>(r.offset + 6));
    }
    return out;
}

}  // namespace patch::text
