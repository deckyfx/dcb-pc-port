// Trainer panel: keyboard-driven search -> freeze -> save -> reload, toggling cheats, error
// statuses, and rendering at small and large sizes (never wider / taller than asked).

#include "settings.hpp"
#include "trainer.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

namespace fs = std::filesystem;
using namespace trainer;

bool contains(const std::vector<Line>& lines, const std::string& text) {
    for (const Line& l : lines)
        if (l.text.find(text) != std::string::npos) return true;
    return false;
}

void check_fits(const Trainer& t, int cols, int rows) {
    const std::vector<Line> lines = t.render(cols, rows);
    CHECK(static_cast<int>(lines.size()) <= rows);
    for (const Line& l : lines) CHECK(static_cast<int>(l.text.size()) <= cols);
}

void test_workflow() {
    const fs::path dir = fs::temp_directory_path() / "dcb_test_trainer_panel";
    fs::remove_all(dir);
    const fs::path file = dir / "cheats" / "TEST.txt";
    std::vector<uint8_t> ram(kRamSize, 0);
    Trainer t(ram.data(), file);
    CHECK(t.load().front().find("no cheat file") != std::string::npos);
    CHECK(t.cheats().cheats().empty());
    t.set_open(true);
    CHECK(t.is_open());
    CHECK(contains(t.render(kPanelCols, kPanelRows), "[General]"));  // the first tab, presets or not
    t.key(Key::Tab);  // Battle
    t.key(Key::Tab);  // Custom
    CHECK(contains(t.render(kPanelCols, kPanelRows), "No cheats"));

    // Search tab: 16-bit (default), value 1234.
    write_value(ram.data(), 0x0B1234, ValueSize::U16, 1234);
    write_value(ram.data(), 0x000100, ValueSize::U16, 1234);
    t.key(Key::Tab);  // Search
    t.text("1234");  // the value row is selected first
    t.key(Key::Enter);
    CHECK(t.search().count() == 2);
    CHECK(t.status().find("2 results") != std::string::npos);
    // The game changes one of them: "changed" narrows to it.
    write_value(ram.data(), 0x0B1234, ValueSize::U16, 1300);
    t.key(Key::Down);  // Filter row
    for (int i = 0; i < 4; ++i) t.key(Key::Right);  // "= value" -> "changed"
    t.key(Key::Enter);
    CHECK(t.search().count() == 1 && t.search().offset(0) == 0x0B1234);
    CHECK(contains(t.render(kPanelCols, kPanelRows), "800B1234  1300"));

    // Select the result, freeze it at 9999, write 42 once.
    t.key(Key::End);
    t.text("q");  // not a command: ignored
    t.key(Key::Up);
    t.key(Key::Up);  // New search row: typing a digit jumps to the value field
    t.key(Key::Up);
    t.key(Key::Delete);  // clear the value
    t.text("9999");
    t.key(Key::End);
    t.text("f");
    CHECK(t.cheats().cheats().size() == 1 && t.cheats().cheats()[0].enabled && t.dirty());
    CHECK(t.cheats().cheats()[0].codes[0] == "800B1234 270F");
    t.apply_frame();
    CHECK(read_value(ram.data(), 0x0B1234, ValueSize::U16) == 9999);
    for (int i = 0; i < 6; ++i) t.key(Key::Up);
    t.key(Key::Down);
    t.key(Key::Down);  // value row
    t.key(Key::Delete);
    t.text("42");
    t.key(Key::End);
    t.text("w");
    CHECK(read_value(ram.data(), 0x0B1234, ValueSize::U16) == 42);
    t.key(Key::Home);
    t.key(Key::Down);
    t.key(Key::Down);
    t.key(Key::Delete);
    t.text("99999");  // too big for 16 bits
    t.key(Key::End);
    t.text("w");
    CHECK(t.status().find("not a 16-bit value") != std::string::npos);
    CHECK(read_value(ram.data(), 0x0B1234, ValueSize::U16) == 42);

    // Cheats tab: save, toggle off (no effect on RAM), reload restores the saved state.
    t.key(Key::Tab);  // General
    t.key(Key::Tab);  // Battle
    t.key(Key::Tab);  // Custom
    t.text("s");
    CHECK(!t.dirty() && fs::exists(file));
    CHECK(platform::read_text_file(file)->find("[Freeze 800B1234 = 9999] on\n800B1234 270F\n") != std::string::npos);
    t.key(Key::Enter);
    CHECK(!t.cheats().cheats()[0].enabled && t.dirty());
    write_value(ram.data(), 0x0B1234, ValueSize::U16, 1);
    t.apply_frame();
    CHECK(read_value(ram.data(), 0x0B1234, ValueSize::U16) == 1);
    t.text("r");
    CHECK(t.cheats().cheats()[0].enabled && !t.dirty());
    // Delete removes it (unsaved until S).
    t.key(Key::Delete);
    CHECK(t.cheats().cheats().empty() && t.dirty());

    // Esc closes.
    t.key(Key::Close);
    CHECK(!t.is_open());
    fs::remove_all(dir);
}

void test_invalid_cheat_and_rendering() {
    const fs::path dir = fs::temp_directory_path() / "dcb_test_trainer_panel2";
    fs::remove_all(dir);
    const fs::path file = dir / "c.txt";
    CHECK(platform::write_text_file(file, "[Good] off\n80000010 0001\n[Bad] on\nC1000000 0001\n"));
    std::vector<uint8_t> ram(kRamSize, 0);
    Trainer t(ram.data(), file);
    const std::vector<std::string> log = t.load();
    CHECK(log.size() == 2 && log[0].find("2 cheats (0 on)") != std::string::npos);
    CHECK(log[1].find("C1") != std::string::npos);
    t.set_open(true);
    t.key(Key::Tab);  // General -> Battle
    t.key(Key::Tab);  // Custom
    t.key(Key::Down);
    t.text(" ");
    CHECK(t.status().find("cannot enable") != std::string::npos);
    CHECK(contains(t.render(kPanelCols, kPanelRows), "[!] Bad"));
    // Every size renders within bounds, including tiny ones, on both tabs, with long results.
    for (int tab = 0; tab < 4; ++tab) {
        for (int cols : {1, 8, 20, 38, 64, 200})
            for (int rows : {1, 4, 12, 22, 30, 80}) check_fits(t, cols, rows);
        t.key(Key::Tab);
    }
    t.key(Key::Tab);  // search tab
    t.text("0");
    t.key(Key::Enter);
    CHECK(t.search().count() == kRamSize / 2);  // all-zero RAM: every 16-bit address matches
    CHECK(contains(t.render(kPanelCols, kPanelRows), "first 500 listed"));
    t.key(Key::PageDown);
    for (int i = 0; i < 100; ++i) t.key(Key::PageDown);
    check_fits(t, kPanelCols, kPanelRows);
    fs::remove_all(dir);
}

// The Battle tab: values are multiples of 10 within the game's caps, the lines round-trip through
// the cheat file next to the cheats (which the cheat parser never sees), bad lines are reported.
void test_battle_tab() {
    CHECK(snap_battle_value(5005, 9990) == 5000);
    CHECK(snap_battle_value(99999, 9990) == 9990);
    CHECK(snap_battle_value(95, 90) == 90);
    CHECK(snap_battle_value(-20, 90) == 0);
    BattleActions actions;
    CHECK(actions.list().size() == 14);
    CHECK(actions.list()[12].id == "p1_win" && !actions.list()[12].has_value() && !actions.list()[12].is_toggle());
    CHECK(actions.list()[13].id == "p2_win" && actions.list()[13].player == 1 &&
          actions.list()[13].label() == "P2 wins at the next battle phase (you lose)");
    CHECK(actions.parse_line("!battle p1_win on 0") && actions.list()[12].enabled && actions.list()[12].value == 0);
    CHECK(actions.list()[10].id == "p1_noshuffle" && actions.list()[10].is_toggle() && actions.list()[10].max() == 0);
    CHECK(actions.list()[11].id == "p2_noshuffle" && !actions.no_shuffle(0) && !actions.no_shuffle(1));
    CHECK(actions.parse_line("!battle p2_noshuffle on 0") && actions.no_shuffle(1) && !actions.no_shuffle(0));
    CHECK(actions.list()[0].id == "p1_hp" && actions.list()[0].value == 9990 && !actions.list()[0].enabled);
    CHECK(actions.list()[4].id == "p1_dp" && actions.list()[4].value == 90);
    CHECK(actions.list()[5].id == "p2_hp" && actions.list()[5].value == 0);
    CHECK(actions.parse_line("!battle p2_circle on 1235") && actions.list()[6].enabled && actions.list()[6].value == 1230);
    CHECK(!actions.parse_line("!battle p3_hp on 10") && !actions.parse_line("!battle p1_hp maybe 10"));
    CHECK(BattleActions::owns_line("  !battle p1_hp on 0") && BattleActions::owns_line("#!battle comment"));
    CHECK(!BattleActions::owns_line("[Cheat] on") && !BattleActions::owns_line("# comment"));

    const fs::path dir = fs::temp_directory_path() / "dcb_test_trainer_battle";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path file = dir / "cheats.txt";
    CHECK(platform::write_text_file(file, "[Keep me] on\n80000100 0001\n!battle p1_dp on 50\n!battle nonsense\n"));
    std::vector<uint8_t> ram(kRamSize, 0);
    Trainer t(ram.data(), file);
    const std::vector<std::string> log = t.load();
    CHECK(t.cheats().cheats().size() == 1 && t.cheats().enabled_count() == 1);
    CHECK(t.battle().list()[4].enabled && t.battle().list()[4].value == 50);
    bool reported = false;
    for (const std::string& line : log) reported = reported || line.find("bad battle line") != std::string::npos;
    CHECK(reported);

    t.set_open(true);
    t.key(Key::Tab);  // General -> Battle, P1 HP selected
    CHECK(contains(t.render(kPanelCols, kPanelRows), "[Battle]"));
    t.text(" ");  // P1 HP on
    CHECK(t.battle().list()[0].enabled);
    t.key(Key::Left);  // -10
    CHECK(t.battle().list()[0].value == 9980);
    t.key(Key::Right);
    t.key(Key::Right);  // capped
    CHECK(t.battle().list()[0].value == 9990);
    t.text("5005");
    t.key(Key::Enter);  // typed value, snapped
    CHECK(t.battle().list()[0].value == 5000 && t.battle().list()[0].enabled);
    for (int i = 0; i < 4; ++i) t.key(Key::Down);  // P1 DP
    t.text("95");
    t.key(Key::Enter);
    CHECK(t.battle().list()[4].value == 90);
    check_fits(t, kPanelCols, kPanelRows);
    check_fits(t, 20, 12);
    // The no-shuffle toggles: Space turns one on; values and typing do nothing there.
    for (int i = 0; i < 6; ++i) t.key(Key::Down);  // P1 deck in order
    t.text(" ");
    t.key(Key::Right);
    t.text("50");
    t.key(Key::Enter);  // nothing typed on a toggle: Enter toggles it back
    CHECK(!t.battle().no_shuffle(0));
    t.text(" ");
    CHECK(t.battle().no_shuffle(0) && t.battle().list()[10].value == 0);
    CHECK(contains(t.render(kPanelCols, kPanelRows), "deck in order"));
    t.text("s");
    CHECK(!t.dirty());
    const std::string saved = *platform::read_text_file(file);
    CHECK(saved.find("!battle p1_noshuffle on 0\n") != std::string::npos);
    CHECK(saved.find("[Keep me] on\n80000100 0001\n") != std::string::npos);
    CHECK(saved.find("!battle p1_hp on 5000\n") != std::string::npos);
    CHECK(saved.find("!battle p1_dp on 90\n") != std::string::npos);
    CHECK(saved.find("nonsense") == std::string::npos);
    // Saving again does not pile up the block.
    Trainer again(ram.data(), file);
    again.load();
    CHECK(again.battle().list()[0].value == 5000 && again.cheats().cheats().size() == 1);
    CHECK(again.save());
    const std::string twice = *platform::read_text_file(file);
    CHECK(twice.find("#!battle") == twice.rfind("#!battle"));
    fs::remove_all(dir);
}

// Presets: built-in cheats at the top of the General tab, toggled, applied, and their state kept
// in the cheat file as "!preset" lines (not as cheats).
void test_presets() {
    const fs::path dir = fs::temp_directory_path() / "dcb_test_trainer_presets";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path file = dir / "cheats.txt";
    CHECK(platform::write_text_file(file, "[Mine] off\n80000200 0002\n!preset Two words on\n"));
    std::vector<uint8_t> ram(kRamSize, 0);
    Trainer t(ram.data(), file);
    t.load();
    t.set_presets("[One] off\n80000100 0001\n[Two words] off\n80000102 0005\n");
    CHECK(t.presets().cheats().size() == 2 && !t.presets().cheats()[0].enabled && t.presets().cheats()[1].enabled);
    CHECK(t.cheats().cheats().size() == 1);  // presets are not custom cheats
    t.apply_frame();
    CHECK(read_value(ram.data(), 0x102, ValueSize::U16) == 5 && read_value(ram.data(), 0x100, ValueSize::U16) == 0);
    t.set_open(true);
    CHECK(contains(t.render(kPanelCols, kPanelRows), "[General]"));
    t.text(" ");  // One on
    CHECK(t.presets().cheats()[0].enabled);
    t.key(Key::Delete);  // presets cannot be removed
    CHECK(t.presets().cheats().size() == 2);
    t.key(Key::Tab);
    CHECK(contains(t.render(kPanelCols, kPanelRows), "[Battle]"));
    t.key(Key::Tab);
    CHECK(contains(t.render(kPanelCols, kPanelRows), "[Custom]"));
    t.key(Key::Tab);
    CHECK(contains(t.render(kPanelCols, kPanelRows), "[Search]"));
    t.key(Key::Tab);  // back to General
    CHECK(contains(t.render(kPanelCols, kPanelRows), "[General]"));
    t.key(Key::Down);
    t.key(Key::Down);  // past the two presets: the first game toggle
    t.text(" ");
    CHECK(t.toggles().on(GameToggle::FusionMutate) && t.presets().cheats()[1].enabled);
    t.text(" ");
    check_fits(t, 20, 12);
    CHECK(t.save());
    const std::string saved = *platform::read_text_file(file);
    CHECK(saved.find("!preset One on\n") != std::string::npos && saved.find("!preset Two words on\n") != std::string::npos);
    CHECK(saved.find("[One]") == std::string::npos);
    Trainer again(ram.data(), file);
    again.set_presets("[One] off\n80000100 0001\n[Two words] off\n80000102 0005\n");
    again.load();  // either order works
    CHECK(again.presets().cheats()[0].enabled && again.presets().cheats()[1].enabled);
    CHECK(again.save());
    const std::string twice = *platform::read_text_file(file);
    CHECK(twice.find("!preset One on") == twice.rfind("!preset One on"));
    fs::remove_all(dir);
}

// Game toggles: listed under the presets on the General tab, kept in the cheat file as "!toggle"
// lines (older files wrote them from the Battle tab: same lines); the fusion roll filter and the
// Digimental flag bits.
void test_game_toggles() {
    GameToggles toggles;
    CHECK(toggles.list().size() == 3 && !toggles.on(GameToggle::FusionMutate));
    CHECK(toggles.list()[0].id == "fusion_mutate" && toggles.list()[2].id == "digimentals");
    CHECK(toggles.parse_line("!toggle fusion_jewel on") && toggles.on(GameToggle::FusionJewel));
    CHECK(!toggles.parse_line("!toggle nonsense on") && !toggles.parse_line("!toggle fusion_mutate maybe"));
    CHECK(!toggles.parse_line("!toggle fusion_mutate on extra") && !toggles.on(GameToggle::FusionMutate));
    CHECK(GameToggles::owns_line(" !toggle digimentals on") && GameToggles::owns_line("#!toggle comment"));
    CHECK(!GameToggles::owns_line("!battle p1_hp on 0") && !BattleActions::owns_line("!toggle digimentals on"));

    // The roll filter: a special fusion is always kept; a mutation is asked for, then a jewel.
    CHECK(fusion_roll_wanted(FusionKind::Special, 0, true, true));
    CHECK(!fusion_roll_wanted(FusionKind::Normal, 50, true, false));
    CHECK(fusion_roll_wanted(FusionKind::Mutation, 200, true, false));
    CHECK(!fusion_roll_wanted(FusionKind::Mutation, 200, true, true));  // Fake Sevens: roll again
    CHECK(fusion_roll_wanted(FusionKind::Mutation, 273, false, true) && fusion_roll_wanted(FusionKind::Mutation, 284, true, true));
    CHECK(!fusion_roll_wanted(FusionKind::Mutation, 285, true, true) && !fusion_roll_wanted(FusionKind::Normal, 273, false, true));
    CHECK(fusion_roll_wanted(FusionKind::Normal, 50, false, false));

    // City flag rN is bit N-12 of the bytes at game_data + 0x23CC (Veemon, r294, is bit 282).
    CHECK(city_flag_bit(12).byte == 0 && city_flag_bit(12).mask == 0x01);
    CHECK(city_flag_bit(294).byte == 35 && city_flag_bit(294).mask == 0x04);
    CHECK(city_flag_bit(267).byte == 31 && city_flag_bit(267).mask == 0x80);
    std::vector<uint8_t> flags(48, 0);
    flags[35] = 0x04;  // Veemon
    CHECK(set_digimental_flags(flags.data()));
    CHECK(flags[35] == (0x04 | 0xB8) && flags[36] == 0x6D && flags[37] == 0x1B);
    CHECK(!set_digimental_flags(flags.data()));  // already set: nothing changes
    for (size_t i = 0; i < flags.size(); ++i)
        if (i < 35 || i > 37) CHECK(flags[i] == 0);

    // In the panel: the General tab's first rows (no presets here); Space switches, S saves, the
    // file keeps them.
    const fs::path dir = fs::temp_directory_path() / "dcb_test_trainer_toggles";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path file = dir / "cheats.txt";
    // An older file: the block header still names the Battle tab.
    CHECK(platform::write_text_file(file, "[Keep me] on\n80000100 0001\n#!toggle Battle tab, game toggles (Fusion Shop, "
                                          "progression flags):\n!toggle digimentals on\n!toggle bad on\n"));
    std::vector<uint8_t> ram(kRamSize, 0);
    Trainer t(ram.data(), file);
    const std::vector<std::string> log = t.load();
    CHECK(t.toggles().on(GameToggle::Digimentals) && t.cheats().cheats().size() == 1);
    bool reported = false;
    for (const std::string& line : log) reported = reported || line.find("bad toggle line") != std::string::npos;
    CHECK(reported);
    t.set_open(true);
    CHECK(contains(t.render(kPanelCols, kPanelRows), "Fusion: every fusion mutates"));
    CHECK(contains(t.render(kPanelCols, kPanelRows), "All Digimentals"));
    CHECK(contains(t.render(kPanelCols, kPanelRows), "[General]"));  // the first toggle is selected
    t.key(Key::Right);                              // no value: nothing happens
    t.text("5");
    t.key(Key::Enter);                              // Enter switches it
    CHECK(t.toggles().on(GameToggle::FusionMutate));
    CHECK(t.battle().list()[13].value == 0 && !t.battle().list()[13].enabled);
    t.key(Key::Down);
    t.key(Key::Down);
    t.text(" ");  // digimentals off
    CHECK(!t.toggles().on(GameToggle::Digimentals));
    check_fits(t, kPanelCols, kPanelRows);
    check_fits(t, 20, 12);
    t.text("s");
    const std::string saved = *platform::read_text_file(file);
    CHECK(saved.find("!toggle fusion_mutate on\n") != std::string::npos);
    CHECK(saved.find("!toggle digimentals off\n") != std::string::npos);
    CHECK(saved.find("[Keep me] on\n") != std::string::npos && saved.find("bad") == std::string::npos);
    CHECK(saved.find("#!toggle General tab") != std::string::npos && saved.find("#!toggle Battle tab") == std::string::npos);
    Trainer again(ram.data(), file);
    again.load();
    CHECK(again.toggles().on(GameToggle::FusionMutate) && !again.toggles().on(GameToggle::Digimentals));
    CHECK(again.save());
    const std::string twice = *platform::read_text_file(file);
    CHECK(twice.find("#!toggle") == twice.rfind("#!toggle"));
    fs::remove_all(dir);
}

}  // namespace

int main() {
    test_workflow();
    test_invalid_cheat_and_rendering();
    test_battle_tab();
    test_presets();
    test_game_toggles();
    std::puts("trainer panel: all tests passed");
    return 0;
}
