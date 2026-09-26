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
    CHECK(contains(t.render(kPanelCols, kPanelRows), "No cheats"));

    // Search tab: 16-bit (default), value 1234.
    write_value(ram.data(), 0x0B1234, ValueSize::U16, 1234);
    write_value(ram.data(), 0x000100, ValueSize::U16, 1234);
    t.key(Key::Tab);
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
    t.key(Key::Tab);
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
    t.key(Key::Down);
    t.text(" ");
    CHECK(t.status().find("cannot enable") != std::string::npos);
    CHECK(contains(t.render(kPanelCols, kPanelRows), "[!] Bad"));
    // Every size renders within bounds, including tiny ones, on both tabs, with long results.
    for (int tab = 0; tab < 2; ++tab) {
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

}  // namespace

int main() {
    test_workflow();
    test_invalid_cheat_and_rendering();
    std::puts("trainer panel: all tests passed");
    return 0;
}
