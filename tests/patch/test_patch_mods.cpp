// Gameplay mods (src/patch/mods.cpp) on a synthetic city script with the Battle Cafe's shape.

#include "patch/mods.hpp"
#include "patch/text_internal.hpp"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

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
Bytes cafe_script(size_t& p_at, size_t& menu_at, size_t& test_at) {
    Bytes s = {'M', 'S', 'C', 'D'};
    patch::wr32(s, 3);
    patch::wr32(s, 0);
    patch::wr32(s, 372);
    cmd(s, 6);
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
        if (rec.op == 5) CHECK(starts.count(target_of(*out, rec.offset)));  // every jump lands on a record
        if (rec.offset < in.size()) continue;
        if (rec.op == 0x0B && patch::rd16(rec.raw, 2) == 2 && patch::rd16(rec.raw, 6) == 140) battle = true;
        if (rec.op == 0x0B && patch::rd16(rec.raw, 2) == 15 && patch::rd16(rec.raw, 6) == 99) music = true;
        if (rec.op == 0x0B && patch::rd16(rec.raw, 2) == 3 && patch::rd16(rec.raw, 6) == 5) listed = true;
    }
    CHECK(battle && music && listed);
    // The record falling into the menu is now a jump; the menu's own record is untouched; the
    // first dispatch test can no longer skip (r2 != r2) and the old jump to the menu moved on.
    CHECK(patch::rd16(*out, p_at) == 5 && target_of(*out, p_at) >= in.size());
    CHECK(Bytes(out->begin() + menu_at, out->begin() + menu_at + 4) == Bytes(in.begin() + menu_at, in.begin() + menu_at + 4));
    CHECK(patch::rd16(*out, test_at + 6) == 1 && patch::rd32(*out, test_at + 8) == 2);
    CHECK(target_of(*out, test_at + 12) >= in.size());
    CHECK(target_of(*out, in.size() - 8) >= in.size());

    // A script without the cafe's shape is left alone.
    Bytes plain(in.begin(), in.begin() + menu_at);
    for (int i = 0; i < 4; ++i) plain[8 + i] = static_cast<uint8_t>(plain.size() >> (8 * i));
    CHECK(!patch::mods::add_rematches(plain, {r}, &why) && !why.empty());
}

void test_table() {
    CHECK(patch::mods::rematches_for("C/AREA05.PAK").size() == 1);
    CHECK(patch::mods::rematches_for("C/AREA11.PAK").front().deck == 140);
    CHECK(patch::mods::rematches_for("C/AREA00.PAK").empty());
}

}  // namespace

int main() {
    test_rematch();
    test_table();
    std::puts("patch mods: all checks passed");
    return 0;
}
