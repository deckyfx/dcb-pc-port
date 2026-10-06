// Gameplay mods (src/patch/mods.cpp) on a synthetic city script with the Battle Cafe's shape.

#include "patch/mods.hpp"
#include "patch/text_internal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

namespace {

using patch::Bytes;

void cmd(Bytes& s, uint16_t c, std::initializer_list<uint16_t> args = {}) {
    patch::wr16(s, static_cast<uint16_t>(0x0A + args.size()));
    patch::wr16(s, c);
    for (const uint16_t a : args) {
        patch::wr16(s, 0);
        patch::wr16(s, a);
    }
}
void skip_if(Bytes& s, uint16_t reg, uint16_t cmp, int32_t v) {
    for (const uint16_t h : {uint16_t{9}, reg, cmp, uint16_t{0}}) patch::wr16(s, h);
    patch::wr32(s, static_cast<uint32_t>(v));
}
void jump(Bytes& s, size_t target) {
    patch::wr16(s, 5);
    patch::wr16(s, 0);
    patch::wr32(s, static_cast<uint32_t>(target - 16));
}
size_t target_of(const Bytes& s, size_t at) { return static_cast<int32_t>(patch::rd32(s, at + 4)) + 16u; }

/// The cafe's shape: a list entry falling into the menu, the menu, the pick and its tests, one
/// opponent section that battles, restores the music and returns, and a jump back to the menu.
/// `gated`: the entry (slot 1) is listed only while r185 != 1 (`skip_if(r185 != 1); jump past`).
Bytes cafe_script(size_t& p_at, size_t& menu_at, size_t& test_at, bool gated = false) {
    Bytes s = {'M', 'S', 'C', 'D'};
    patch::wr32(s, 3);
    patch::wr32(s, 0);
    patch::wr32(s, 372);
    cmd(s, 6);
    if (gated) {
        skip_if(s, 185, 3, 1);
        jump(s, s.size() + 8 + 8);  // past the entry, onto the menu
    }
    p_at = s.size();
    cmd(s, 3, {1});
    menu_at = s.size();
    cmd(s, 2);
    const size_t ret = s.size();
    cmd(s, 6);
    cmd(s, 3);
    test_at = s.size();
    skip_if(s, 2, 3, 1);
    const size_t to_section = s.size();
    jump(s, 0x10);
    skip_if(s, 2, 3, -1);
    jump(s, 0x10);
    const size_t section = s.size();
    cmd(s, 2, {77});
    cmd(s, 15, {99});
    jump(s, ret);
    jump(s, menu_at);
    const uint32_t rel = static_cast<uint32_t>(section - 16);
    for (int i = 0; i < 4; ++i) s[to_section + 4 + i] = static_cast<uint8_t>(rel >> (8 * i));
    for (int i = 0; i < 4; ++i) s[8 + i] = static_cast<uint8_t>(s.size() >> (8 * i));
    return s;
}

void test_rematch() {
    size_t p_at = 0, menu_at = 0, test_at = 0;
    const Bytes in = cafe_script(p_at, menu_at, test_at);
    patch::mods::Rematch r;
    r.slot = 5;
    r.deck = 140;
    r.unlocked = {{185, 1}, {184, 0}};
    r.name = "A";
    r.challenge = "Again?";
    r.declined = "No.";
    r.player_won = "Won.";
    r.player_lost = "Lost.";
    std::string why;
    const auto out = patch::mods::add_rematches(in, {r}, &why);
    CHECK(out);
    CHECK(patch::rd32(*out, 8) == out->size());

    const auto recs = patch::text::msd_walk(*out);
    std::set<size_t> starts;
    for (const auto& rec : recs) starts.insert(rec.offset);
    bool battle = false, music = false, listed = false;
    for (const auto& rec : recs) {
        if (rec.op == 5) CHECK(starts.count(target_of(*out, rec.offset)) || target_of(*out, rec.offset) == out->size());  // on a record, or the end
        if (rec.offset < in.size()) continue;
        if (rec.op == 0x0B && patch::rd16(rec.raw, 2) == 2 && patch::rd16(rec.raw, 6) == 140) battle = true;
        if (rec.op == 0x0B && patch::rd16(rec.raw, 2) == 15 && patch::rd16(rec.raw, 6) == 99) music = true;
        if (rec.op == 0x0B && patch::rd16(rec.raw, 2) == 3 && patch::rd16(rec.raw, 6) == 5) listed = true;
    }
    CHECK(battle && music && listed);
    // Running off the original last record still ends the script: the new code starts with a
    // jump to the (new) end.
    CHECK(patch::rd16(*out, in.size()) == 5 && target_of(*out, in.size()) == out->size());
    // The record falling into the menu is now a jump; the menu's own record is untouched; the
    // first dispatch test can no longer skip (r2 != r2) and the old jump to the menu moved on.
    CHECK(patch::rd16(*out, p_at) == 5 && target_of(*out, p_at) >= in.size());
    CHECK(Bytes(out->begin() + menu_at, out->begin() + menu_at + 4) == Bytes(in.begin() + menu_at, in.begin() + menu_at + 4));
    CHECK(patch::rd16(*out, test_at + 6) == 1 && patch::rd32(*out, test_at + 8) == 2);
    CHECK(target_of(*out, test_at + 12) >= in.size());
    CHECK(target_of(*out, in.size() - 8) >= in.size());

    // The cafe lists slot 1 for a regular member: a boss may not take it.
    r.slot = 1;
    CHECK(!patch::mods::add_rematches(in, {r}, &why) && why.find("slot 1") != std::string::npos);
    // Unless that entry is listed only until the boss is unlocked (Wormmon's deck-info entry).
    size_t gp = 0, gm = 0, gt = 0;
    const Bytes gated = cafe_script(gp, gm, gt, true);
    CHECK(patch::mods::add_rematches(gated, {r}, &why));
    r.slot = 5;

    // A script without the cafe's shape is left alone.
    Bytes plain(in.begin(), in.begin() + menu_at);
    for (int i = 0; i < 4; ++i) plain[8 + i] = static_cast<uint8_t>(plain.size() >> (8 * i));
    CHECK(!patch::mods::add_rematches(plain, {r}, &why) && !why.empty());
}

void arith(Bytes& s, uint16_t reg, int32_t v) {
    for (const uint16_t h : {uint16_t{7}, reg, uint16_t{0}, uint16_t{0}}) patch::wr16(s, h);
    patch::wr32(s, static_cast<uint32_t>(v));
}

/// An arena group: per battle the set-up (name boxes, r10), a Battle / Deck info menu with its
/// tests, and a battle (`deck`); then the group's save block (cmd6(3), music 125).
Bytes arena_script(std::vector<size_t>& setups, std::vector<size_t>& item14s, const std::vector<uint16_t>& decks) {
    Bytes s = {'M', 'S', 'C', 'D'};
    patch::wr32(s, 3);
    patch::wr32(s, 0);
    patch::wr32(s, 372);
    std::vector<size_t> fix;  // jumps to the record after the group (filled below)
    for (const uint16_t deck : decks) {
        setups.push_back(s.size());
        cmd(s, 0, {0, 128, 10});
        cmd(s, 0, {1, 128, 10});
        arith(s, 10, 1);
        cmd(s, 13);
        if (deck == 9)  // a story fight: a long intro between the set-up and the menu
            for (int k = 0; k < 100; ++k) cmd(s, 4);
        cmd(s, 0, {97});
        cmd(s, 1, {13});
        item14s.push_back(s.size());
        cmd(s, 1, {14});
        cmd(s, 1);
        skip_if(s, 1, 3, 1);
        const size_t to_battle = s.size();
        jump(s, 16);
        skip_if(s, 1, 3, 2);
        fix.push_back(s.size());
        jump(s, 16);
        skip_if(s, 1, 3, -1);
        fix.push_back(s.size());
        jump(s, 16);
        const uint32_t rel = static_cast<uint32_t>(s.size() - 16);
        for (int i = 0; i < 4; ++i) s[to_battle + 4 + i] = static_cast<uint8_t>(rel >> (8 * i));
        cmd(s, 2, {deck});
        fix.push_back(s.size());
        jump(s, 16);
    }
    cmd(s, 0, {0, 128, 10});
    cmd(s, 0, {1, 128, 10});
    cmd(s, 6);
    cmd(s, 15, {110});
    cmd(s, 6, {3});
    cmd(s, 15, {125});
    jump(s, setups.front());
    const uint32_t end = static_cast<uint32_t>(s.size() - 16);
    cmd(s, 6);
    for (const size_t f : fix)
        for (int i = 0; i < 4; ++i) s[f + 4 + i] = static_cast<uint8_t>(end >> (8 * i));
    for (int i = 0; i < 4; ++i) s[8 + i] = static_cast<uint8_t>(s.size() >> (8 * i));
    return s;
}

/// Every jump lands on a record start, or the end of the script.
void check_jumps(const Bytes& out) {
    const auto recs = patch::text::msd_walk(out);
    std::set<size_t> starts;
    for (const auto& rec : recs) starts.insert(rec.offset);
    for (const auto& rec : recs)
        if (rec.op == 5) CHECK(starts.count(target_of(out, rec.offset)) || target_of(out, rec.offset) == out.size());
}

/// The menu's Deck info item (at `at`) is a jump to items 14 and 15 (Save).
bool has_save_item(const Bytes& out, size_t at) {
    if (patch::rd16(out, at) != 5) return false;
    const size_t items = target_of(out, at);
    return patch::rd16(out, items) == 0x0B && patch::rd16(out, items + 6) == 14 && patch::rd16(out, items + 8) == 0x0B &&
           patch::rd16(out, items + 14) == 15;
}

/// The appended save blocks: each runs the group's save (cmd6(3), music 125) then jumps to one of
/// `setups`; returns how many.
size_t save_blocks(const Bytes& out, size_t from, const std::vector<size_t>& setups) {
    const auto recs = patch::text::msd_walk(out);
    size_t n = 0;
    for (size_t k = 0; k + 2 < recs.size(); ++k) {
        const auto& r = recs[k];
        if (r.offset < from || !(r.op == 0x0B && patch::rd16(r.raw, 2) == 6 && patch::rd16(r.raw, 6) == 3)) continue;
        CHECK(recs[k + 1].op == 0x0B && patch::rd16(recs[k + 1].raw, 6) == 125);
        CHECK(recs[k + 2].op == 5);
        const size_t to = target_of(out, recs[k + 2].offset);
        CHECK(std::find(setups.begin(), setups.end(), to) != setups.end());
        ++n;
    }
    return n;
}

void test_arena_saves() {
    std::vector<size_t> setups, item14s;
    const Bytes in = arena_script(setups, item14s, {7, 9, 140});
    std::string why;
    const auto out = patch::mods::add_arena_saves(in, &why);
    CHECK(out);
    check_jumps(*out);
    // Both regular battles get Save, also the one with a long intro; A's (deck 140) is untouched.
    CHECK(has_save_item(*out, item14s[0]) && has_save_item(*out, item14s[1]));
    CHECK(Bytes(out->begin() + item14s[2], out->begin() + item14s[2] + 8) ==
          Bytes(in.begin() + item14s[2], in.begin() + item14s[2] + 8));
    // One save block per patched battle, each back to a battle's set-up.
    CHECK(save_blocks(*out, in.size(), {setups[0], setups[1]}) == 2);
    // A script without an arena menu is left alone.
    Bytes plain(in.begin(), in.begin() + 16);
    cmd(plain, 6);
    for (int i = 0; i < 4; ++i) plain[8 + i] = static_cast<uint8_t>(plain.size() >> (8 * i));
    CHECK(!patch::mods::add_arena_saves(plain, &why) && !why.empty());
}

/// A city menu (cmd0(120), the places, the pick, a test per position and -1) for `places`.
Bytes city_script(const std::vector<uint16_t>& places, size_t& last_item, size_t& first_test) {
    Bytes s = {'M', 'S', 'C', 'D'};
    patch::wr32(s, 3);
    patch::wr32(s, 0);
    patch::wr32(s, 372);
    cmd(s, 6);
    cmd(s, 0, {120});
    for (const uint16_t v : places) {
        last_item = s.size();
        cmd(s, 1, {v});
    }
    cmd(s, 1);
    first_test = s.size();
    for (int32_t v = 1; v <= static_cast<int32_t>(places.size()); ++v) {
        skip_if(s, 1, 3, v);
        jump(s, 16);
    }
    skip_if(s, 1, 3, -1);
    jump(s, 16);
    for (int i = 0; i < 4; ++i) s[8 + i] = static_cast<uint8_t>(s.size() >> (8 * i));
    return s;
}

void test_player_rooms() {
    size_t last = 0, test = 0;
    const Bytes in = city_script({2, 3, 1}, last, test);
    std::string why;
    const auto out = patch::mods::add_player_rooms(in, &why);
    CHECK(out);
    check_jumps(*out);
    // The last place is a jump to it and Player Rooms (item 0); the new 4th position opens them.
    CHECK(patch::rd16(*out, last) == 5);
    const size_t items = target_of(*out, last);
    CHECK(patch::rd16(*out, items + 6) == 1 && patch::rd16(*out, items + 8) == 0x0B && patch::rd16(*out, items + 14) == 0);
    bool rooms = false;
    for (const auto& rec : patch::text::msd_walk(*out))
        if (rec.offset >= in.size() && rec.op == 0x0A && patch::rd16(rec.raw, 2) == 7) rooms = true;
    CHECK(rooms);
    // A full menu (five rows) and one that lists Player Rooms already are left alone.
    CHECK(!patch::mods::add_player_rooms(city_script({2, 3, 5, 4, 1}, last, test), &why));
    CHECK(!patch::mods::add_player_rooms(city_script({2, 3, 1, 0}, last, test), &why));
}

void test_postgame() {
    // Beginner City's start: cmd16() (r1 = wins), skip_if(r1 >= 300), jump, the flag, r360 = 1.
    Bytes s = {'M', 'S', 'C', 'D'};
    patch::wr32(s, 3);
    patch::wr32(s, 0);
    patch::wr32(s, 372);
    cmd(s, 16);
    const size_t wins = s.size();
    skip_if(s, 1, 5, 300);
    jump(s, 16);
    skip_if(s, 89, 3, 0);
    jump(s, 16);
    arith(s, 360, 1);
    for (int i = 0; i < 4; ++i) s[8 + i] = static_cast<uint8_t>(s.size() >> (8 * i));
    std::string why;
    const auto out = patch::mods::drop_win_requirements(s, &why);
    CHECK(out && out->size() == s.size());
    CHECK(patch::rd32(*out, wins + 8) == 0);  // r1 >= 0: no wins needed
    CHECK(Bytes(out->begin() + wins + 12, out->end()) == Bytes(s.begin() + wins + 12, s.end()));  // the story flag stays
    // Not the desert city (no r355 test): left alone.
    CHECK(!patch::mods::add_desert_visitors(s, &why));
}

void test_table() {
    CHECK(patch::mods::rematches_for("C/AREA05.PAK").size() == 1);
    CHECK(patch::mods::rematches_for("C/AREA11.PAK").front().deck == 140);
    CHECK(patch::mods::rematches_for("C/AREA00.PAK").empty());
}

}  // namespace

int main() {
    test_rematch();
    test_arena_saves();
    test_player_rooms();
    test_postgame();
    test_table();
    std::puts("patch mods: all checks passed");
    return 0;
}
