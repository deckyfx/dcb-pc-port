#include "patch/mods.hpp"

#include "text_internal.hpp"

#include <algorithm>
#include <initializer_list>
#include <map>
#include <stdexcept>

namespace patch::mods {

namespace {

using text::MsdRecord;

constexpr size_t kHeader = 16;        // jump targets are stored relative to the end of the header
constexpr uint16_t kOpJump = 5, kOpArith = 7, kOpText = 8, kOpSkipIf = 9, kOpCmd0 = 0x0A;
constexpr uint16_t kCmpEq = 0, kCmpNe = 3;  // op 9 comparisons: skip the next record when true
constexpr uint16_t kRegChoice = 1;    // r1: the Yes / No choice, then the battle result (0 = lost)
constexpr uint16_t kRegPick = 2;      // r2: the cafe menu's pick

uint16_t cmd_of(const MsdRecord& r) { return r.op >= kOpCmd0 ? rd16(r.raw, 2) : 0xFFFF; }
uint16_t arg_of(const MsdRecord& r, size_t i) { return rd16(r.raw, 6 + 4 * i); }
size_t jump_target(View script, const MsdRecord& r) {
    return static_cast<size_t>(static_cast<int32_t>(rd32(script, r.offset + 4))) + kHeader;
}
void put16(Bytes& b, size_t at, uint16_t v) {
    b[at] = static_cast<uint8_t>(v);
    b[at + 1] = static_cast<uint8_t>(v >> 8);
}
void put32(Bytes& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

/// Writes MSD records at the end of a script (docs/re/text-engine.md §5.1 for the encodings).
class Assembler {
public:
    explicit Assembler(Bytes& script) : s_(script) {}
    size_t here() const { return s_.size(); }

    /// Host command `cmd` with literal arguments (op 0x0A + argument count).
    void cmd(uint16_t cmd, std::initializer_list<uint16_t> args = {}) {
        wr16(s_, static_cast<uint16_t>(kOpCmd0 + args.size()));
        wr16(s_, cmd);
        for (const uint16_t a : args) {
            wr16(s_, 0);  // a literal, not a register
            wr16(s_, a);
        }
    }
    /// Text into register r4 (the city host's text register), then shown (host 0x0A cmd 4).
    void line(const std::string& text) {
        wr16(s_, kOpText);
        wr16(s_, 4);
        wr16(s_, static_cast<uint16_t>(text.size() + 1));
        s_.insert(s_.end(), text.begin(), text.end());
        s_.push_back(0);
        while (s_.size() % 4) s_.push_back(0);
        cmd(4);
    }
    /// Skip the next record when r`reg` <cmp> `value`.
    void skip_if(uint16_t reg, uint16_t cmp, int32_t value) {
        wr16(s_, kOpSkipIf);
        wr16(s_, reg);
        wr16(s_, cmp);
        wr16(s_, 0);
        wr32(s_, static_cast<uint32_t>(value));
    }
    /// A jump; returns where its target goes (for set_target) when `target` is not known yet.
    size_t jump(size_t target = kHeader) {
        const size_t at = here();
        wr16(s_, kOpJump);
        wr16(s_, 0);
        wr32(s_, static_cast<uint32_t>(target - kHeader));
        return at;
    }
    void set_target(size_t jump_at, size_t target) { put32(s_, jump_at + 4, static_cast<uint32_t>(target - kHeader)); }
    void raw(View record) { s_.insert(s_.end(), record.begin(), record.end()); }

    /// Opens a message box with the speaker's name line, then `text` (one line per '\n'), as the
    /// cafe opponents speak; the caller closes it (host 0x0A cmd 5) or asks a question.
    void say(const std::string& name, const std::string& text) {
        cmd(0, {0, 48, 10});  // name box positions, as every cafe dialogue sets them
        cmd(0, {1, 128, 10});
        cmd(6);
        line("*c4" + name + "*c7");
        size_t start = 0;
        for (size_t nl; (nl = text.find('\n', start)) != std::string::npos; start = nl + 1) line(text.substr(start, nl - start));
        line(text.substr(start));
    }
    void speak(const std::string& name, const std::string& text) {
        say(name, text);
        cmd(5);
    }

private:
    Bytes& s_;
};

/// The Battle Cafe's records in a city script.
struct Cafe {
    size_t pick = 0;          ///< index of `cmd3()` (the pick into r2)
    size_t menu = 0;          ///< index of `cmd2()` (shows the list)
    size_t returns = 0;       ///< jumps back to the record after the menu (opponent sections end so)
    std::optional<uint16_t> music;  ///< host cmd 15 argument after a cafe battle (the cafe's music)
};

std::optional<Cafe> find_cafe(View script, const std::vector<MsdRecord>& recs, std::string* why) {
    std::optional<Cafe> best;
    for (size_t i = 1; i + 2 < recs.size(); ++i) {
        // The pick: `cmd3()` followed by tests of r2 (each `skip_if(r2 != k)` + jump).
        if (!(recs[i].op == kOpCmd0 && cmd_of(recs[i]) == 3)) continue;
        if (!(recs[i + 1].op == kOpSkipIf && rd16(recs[i + 1].raw, 2) == kRegPick && recs[i + 2].op == kOpJump)) continue;
        size_t m = i;
        while (m > 0 && !(recs[m].op == kOpCmd0 && cmd_of(recs[m]) == 2)) --m;
        if (m == 0 || i - m > 8) continue;
        Cafe c{i, m, 0, std::nullopt};
        const size_t after_menu = recs[m + 1].offset;
        for (const MsdRecord& r : recs)
            if (r.op == kOpJump && jump_target(script, r) == after_menu) ++c.returns;
        if (!best || c.returns > best->returns) best = c;
    }
    if (!best || best->returns == 0) {
        if (why) *why = "no Battle Cafe menu";
        return std::nullopt;
    }
    for (size_t i = best->pick; i + 1 < recs.size(); ++i) {
        if (recs[i].op == kOpCmd0 + 1 && cmd_of(recs[i]) == 2) {  // the first cafe battle
            if (recs[i + 1].op == kOpCmd0 + 1 && cmd_of(recs[i + 1]) == 15) best->music = arg_of(recs[i + 1], 0);
            break;
        }
    }
    return best;
}

}  // namespace

const std::vector<Rematch>& rematches_for(const std::string& file) {
    static const std::map<std::string, std::vector<Rematch>> table = {
        {"C/AREA02.PAK",
         {{15, 10, {{56, 1}}, "Wormmon",
           "Want to try my Tryout Deck\nonce more?",
           "Oh... okay. Maybe next time.",
           "You're really strong...\nI'll keep practicing!",
           "I did it! The Wormmon Card\nisn't so weak after all!"}}},
        {"C/AREA03.PAK",
         {{14, 14, {{87, 1}}, "Stingmon",
           "You again? Fine.\nThe Black Storm awaits.",
           "Then get out of my way.",
           "Hmph... You got lucky.",
           "Is that all you've got?"}}},
        {"C/AREA04.PAK",
         {{6, 18, {{119, 1}}, "Shadramon",
           "Back for more?\nMy Evil Fire still burns!",
           "Scared of getting burned?",
           "Grrr... The fire went out...",
           "Burn to ashes!"}}},
        {"C/AREA05.PAK",
         {{11, 23, {{138, 1}}, "Digimon Emperor",
           "So you want to face me again?\nVery well.",
           "Hmph. I knew you were scared.",
           "This isn't over...\nI'll be waiting right here.",
           "Kneel before the\nDigimon Emperor!"}}},
        {"C/AREA11.PAK",
         {{14, 140, {{185, 1}, {184, 0}}, "A",
           "*c6You want to play again?\n*c6I have nothing but time.",
           "*c6Then leave me be.",
           "*c6...Again? How amusing.\n*c6Come back any time.",
           "*c6Did you really think\n*c6you could win?"}}},
    };
    static const std::vector<Rematch> none;
    const auto it = table.find(file);
    return it == table.end() ? none : it->second;
}

std::optional<Bytes> add_rematches(View script, const std::vector<Rematch>& list, std::string* why) {
    std::vector<MsdRecord> recs;
    try {
        recs = text::msd_walk(script);
    } catch (const std::exception& e) {
        if (why) *why = e.what();
        return std::nullopt;
    }
    const std::optional<Cafe> cafe = find_cafe(script, recs, why);
    if (!cafe) return std::nullopt;
    const MsdRecord& menu = recs[cafe->menu];
    const MsdRecord& before = recs[cafe->menu - 1];     // falls into the menu unless it is a jump
    const MsdRecord& test = recs[cafe->pick + 1];       // the first `skip_if(r2 != k)`
    const MsdRecord& test_jump = recs[cafe->pick + 2];  // its jump to opponent k
    const size_t menu_at = menu.offset, after_menu = recs[cafe->menu + 1].offset;
    const size_t after_test = recs[cafe->pick + 3].offset;
    const bool redirect_before = before.op != kOpJump;
    if (redirect_before && (before.raw.size() != 8 || recs[cafe->menu - 2].op == kOpSkipIf)) {
        if (why) *why = "the record before the cafe menu cannot be redirected";
        return std::nullopt;
    }
    if (rd16(test.raw, 4) != kCmpNe || rd16(test.raw, 6) != 0) {
        if (why) *why = "unexpected cafe dispatch test";
        return std::nullopt;
    }
    // A cafe gains members as the story goes on (each behind its own flags): a boss's slot must be
    // one the script never lists, or listed only until the boss is unlocked (Wormmon's deck-info
    // entry, `skip_if(r56 != 1); jump past` = listed while r56 != 1), so neither replaces the other.
    for (size_t i = 0; i < recs.size(); ++i) {
        if (!(recs[i].op == kOpCmd0 + 1 && cmd_of(recs[i]) == 3)) continue;
        for (const Rematch& r : list) {
            if (arg_of(recs[i], 0) != r.slot) continue;
            const MsdRecord& gate = recs[i < 2 ? i : i - 2];
            const bool until_unlocked =
                i >= 2 && gate.op == kOpSkipIf && rd16(gate.raw, 4) == kCmpNe && rd16(gate.raw, 6) == 0 &&
                recs[i - 1].op == kOpJump && jump_target(script, recs[i - 1]) > recs[i].offset &&
                std::any_of(r.unlocked.begin(), r.unlocked.end(), [&](const RegEquals& c) {
                    return rd16(gate.raw, 2) == c.reg && static_cast<int32_t>(rd32(gate.raw, 8)) == c.value;
                });
            if (!until_unlocked) {
                if (why) *why = "cafe slot " + std::to_string(r.slot) + " is listed for someone else";
                return std::nullopt;
            }
        }
    }

    Bytes out(script.begin(), script.end());
    Assembler a(out);

    // The scripts end by running off their last record (leaving a city: Jungle City's exit path
    // plays the map music, sets r0 and falls off the end). The new code starts with a jump to the
    // new end, so running off the old end still ends the script.
    const size_t end_jump = a.jump();

    // The list: the record that fell into the menu (moved here), then each boss when unlocked.
    const size_t moved_before = a.here();
    if (redirect_before) a.raw(before.raw);
    const size_t list_at = a.here();
    for (const Rematch& r : list) {
        std::vector<size_t> skips;
        for (const RegEquals& c : r.unlocked) {
            a.skip_if(c.reg, kCmpEq, c.value);  // equal: skip the jump past the entry
            skips.push_back(a.jump());
        }
        a.cmd(3, {r.slot});
        for (const size_t j : skips) a.set_target(j, a.here());
    }
    a.jump(menu_at);

    // The dispatch: the original first test, then the bosses' slots, then the rest of the chain.
    const size_t dispatch_at = a.here();
    a.raw(test.raw);
    a.jump(jump_target(script, test_jump));
    std::vector<size_t> section_jumps;
    for (const Rematch& r : list) {
        // Only when unlocked: a slot the cafe also lists for something else (Wormmon's deck-info
        // entry before he is beaten) keeps going to its own section.
        std::vector<size_t> skips;
        a.skip_if(kRegPick, kCmpEq, r.slot);
        skips.push_back(a.jump());
        for (const RegEquals& c : r.unlocked) {
            a.skip_if(c.reg, kCmpEq, c.value);
            skips.push_back(a.jump());
        }
        section_jumps.push_back(a.jump());
        for (const size_t j : skips) a.set_target(j, a.here());
    }
    a.jump(after_test);

    // One section per boss: Yes / No, the battle, a line after it, back to the cafe menu.
    for (size_t k = 0; k < list.size(); ++k) {
        const Rematch& r = list[k];
        a.set_target(section_jumps[k], a.here());
        a.say(r.name, r.challenge);
        a.cmd(0, {97});  // the Yes / No list the cafe opponents ask with (items 16 and 17)
        a.cmd(1, {16});
        a.cmd(1, {17});
        a.cmd(1);
        a.skip_if(kRegChoice, kCmpNe, 1);  // Yes (1): jump to the battle
        const size_t to_battle = a.jump();
        a.speak(r.name, r.declined);
        a.jump(after_menu);
        a.set_target(to_battle, a.here());
        a.cmd(2, {r.deck});
        if (cafe->music) a.cmd(15, {*cafe->music});
        a.cmd(14, {60});
        a.skip_if(kRegChoice, kCmpNe, 0);  // lost (0): jump to the losing line
        const size_t to_lost = a.jump();
        a.speak(r.name, r.player_won);
        a.jump(after_menu);
        a.set_target(to_lost, a.here());
        a.speak(r.name, r.player_lost);
        a.jump(after_menu);
    }

    a.set_target(end_jump, a.here());

    // In place, same sizes: every way into the menu now goes through the list.
    for (const MsdRecord& r : recs) {
        if (r.op == kOpJump && jump_target(script, r) == menu_at) put32(out, r.offset + 4, static_cast<uint32_t>(list_at - kHeader));
    }
    if (redirect_before) {
        put16(out, before.offset, kOpJump);
        put16(out, before.offset + 2, 0);
        put32(out, before.offset + 4, static_cast<uint32_t>(moved_before - kHeader));
    }
    // The first test never skips (r2 != r2), so its jump always runs: into the new dispatch.
    put16(out, test.offset + 6, 1);
    put32(out, test.offset + 8, kRegPick);
    put32(out, test_jump.offset + 4, static_cast<uint32_t>(dispatch_at - kHeader));
    put32(out, 8, static_cast<uint32_t>(out.size()));  // header: script size
    return out;
}

std::optional<Bytes> add_arena_saves(View script, std::string* why) {
    std::vector<MsdRecord> recs;
    try {
        recs = text::msd_walk(script);
    } catch (const std::exception& e) {
        if (why) *why = e.what();
        return std::nullopt;
    }
    const auto op_cmd = [&](size_t i, uint16_t op, uint16_t c) { return i < recs.size() && recs[i].op == op && cmd_of(recs[i]) == c; };
    const auto is_test = [&](size_t i, uint16_t reg, int32_t value) {
        return i + 1 < recs.size() && recs[i].op == kOpSkipIf && rd16(recs[i].raw, 2) == reg && rd16(recs[i].raw, 4) == kCmpNe &&
               rd16(recs[i].raw, 6) == 0 && static_cast<int32_t>(rd32(recs[i].raw, 8)) == value && recs[i + 1].op == kOpJump;
    };
    std::map<size_t, size_t> index;  // record offset -> index
    for (size_t i = 0; i < recs.size(); ++i) index[recs[i].offset] = i;

    Bytes out(script.begin(), script.end());
    Assembler a(out);
    const size_t end_jump = a.jump();  // running off the old end still ends the script
    size_t added = 0;
    for (size_t i = 0; i + 10 < recs.size(); ++i) {
        // An arena battle menu: cmd0(97), the items Battle (13) and Deck info (14) only, the pick
        // into r1, then tests for 1, 2 and -1 (cancel).
        if (!(op_cmd(i, kOpCmd0 + 1, 0) && arg_of(recs[i], 0) == 97 && op_cmd(i + 1, kOpCmd0 + 1, 1) &&
              arg_of(recs[i + 1], 0) == 13 && op_cmd(i + 2, kOpCmd0 + 1, 1) && arg_of(recs[i + 2], 0) == 14 &&
              op_cmd(i + 3, kOpCmd0, 1) && is_test(i + 4, kRegChoice, 1) && is_test(i + 6, kRegChoice, 2) &&
              is_test(i + 8, kRegChoice, -1)))
            continue;
        // Not A's fight (the Battle choice leads to deck 140 before the next record that leaves):
        // it changes the whole screen's colours, so it keeps the game's own flow.
        bool boss = false;
        if (const auto it = index.find(jump_target(script, recs[i + 5])); it != index.end()) {
            for (size_t k = it->second; k < recs.size() && k < it->second + 64; ++k) {
                if (op_cmd(k, kOpCmd0 + 1, 2)) {
                    boss = arg_of(recs[k], 0) == 140;
                    break;
                }
            }
        }
        if (boss) continue;
        // The battle's set-up (opponent number r10, name r9, the portrait host cmd 13): where a
        // save made here resumes, as registers below r12 are not saved.
        size_t setup = 0;
        for (size_t k = i; k > 0 && k + 64 > i; --k) {
            if (recs[k].op == kOpArith && rd16(recs[k].raw, 2) == 10 && rd16(recs[k].raw, 4) == 0) {
                setup = k;
                break;
            }
        }
        if (setup == 0) continue;
        while (setup > 0 && op_cmd(setup - 1, kOpCmd0 + 3, 0)) --setup;  // the name box positions before it
        // The arena's own save block (its location code and music): the next one after the menu.
        size_t save = i;
        while (save < recs.size() && !(op_cmd(save, kOpCmd0 + 1, 6) && op_cmd(save - 1, kOpCmd0 + 1, 15) &&
                                       arg_of(recs[save - 1], 0) == 110 && arg_of(recs[save], 0) != 1))
            ++save;
        if (save >= recs.size() || !op_cmd(save + 1, kOpCmd0 + 1, 15)) continue;
        size_t save_start = save - 1;
        while (save_start > 0 && (op_cmd(save_start - 1, kOpCmd0 + 3, 0) || op_cmd(save_start - 1, kOpCmd0, 6))) --save_start;

        // Items: 13, 14, then 15 (Save), back to the pick.
        const MsdRecord& item14 = recs[i + 2];
        const size_t items_at = a.here();
        a.raw(item14.raw);
        a.cmd(1, {15});
        a.jump(recs[i + 3].offset);
        // Save: the arena's save records (cmd6(code) stores where a save resumes: right after it),
        // the arena music, then the battle's set-up again.
        const size_t save_at = a.here();
        for (size_t k = save_start; k <= save + 1; ++k) a.raw(recs[k].raw);
        a.jump(recs[setup].offset);
        // Dispatch: the original first test, then r1 == 3 (Save), then the rest of the tests.
        const size_t dispatch_at = a.here();
        a.raw(recs[i + 4].raw);
        a.jump(jump_target(script, recs[i + 5]));
        a.skip_if(kRegChoice, kCmpNe, 3);
        a.jump(save_at);
        a.jump(recs[i + 6].offset);

        put16(out, item14.offset, kOpJump);
        put16(out, item14.offset + 2, 0);
        put32(out, item14.offset + 4, static_cast<uint32_t>(items_at - kHeader));
        put16(out, recs[i + 4].offset + 6, 1);  // the first test never skips (r1 != r1) ...
        put32(out, recs[i + 4].offset + 8, kRegChoice);
        put32(out, recs[i + 5].offset + 4, static_cast<uint32_t>(dispatch_at - kHeader));  // ... into the new dispatch
        ++added;
    }
    if (added == 0) {
        if (why) *why = "no arena battle menu";
        return std::nullopt;
    }
    a.set_target(end_jump, a.here());
    put32(out, 8, static_cast<uint32_t>(out.size()));
    return out;
}

std::optional<Bytes> patch_city_pak(View pak, const CityMods& mods, std::string* why) {
    std::vector<PakChunk> chunks;
    try {
        chunks = read_pak(pak);
    } catch (const std::exception& e) {
        if (why) *why = e.what();
        return std::nullopt;
    }
    for (PakChunk& c : chunks) {
        if (c.data.size() < 4 || std::string_view(reinterpret_cast<const char*>(c.data.data()), 4) != "MSCD") continue;
        // Each mod on its own: one that does not fit this script leaves it to the others.
        bool changed = false;
        std::string reasons;
        if (!mods.rematches.empty()) {
            std::string w;
            if (std::optional<Bytes> s = add_rematches(c.data, mods.rematches, &w)) {
                c.data = std::move(*s);
                changed = true;
            } else {
                reasons += "boss rematch: " + w;
            }
        }
        if (mods.arena_saves) {
            std::string w;
            if (std::optional<Bytes> s = add_arena_saves(c.data, &w)) {
                c.data = std::move(*s);
                changed = true;
            } else {
                reasons += std::string(reasons.empty() ? "" : "; ") + "arena saves: " + w;
            }
        }
        if (why) *why = reasons;
        if (!changed) return std::nullopt;
        return write_pak(chunks);
    }
    if (why) *why = "no script chunk";
    return std::nullopt;
}

}  // namespace patch::mods
