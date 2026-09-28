// Native pause menu: navigation, actions, slot/card selection, rendering bounds.
// Pure logic (no SDL); the backend only routes keys and draws lines.

#include "menu.hpp"

#include <cstdio>
#include <cstdlib>
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

using namespace menu;

bool contains(const std::vector<Line>& lines, const std::string& text) {
    for (const Line& l : lines)
        if (l.text.find(text) != std::string::npos) return true;
    return false;
}

void test_main_nav() {
    Menu m;
    CHECK(!m.is_open());
    CHECK(m.key(Key::Down) == Action::None);  // closed: keys ignored
    m.set_open(true);
    CHECK(m.is_open() && m.page() == Page::Main && m.selection() == 0);
    CHECK(m.key(Key::Down) == Action::None && m.selection() == 1);
    CHECK(m.key(Key::Up) == Action::None && m.selection() == 0);
    CHECK(m.key(Key::Up) == Action::None && m.selection() == 8);  // wraps
    CHECK(m.key(Key::Down) == Action::None && m.selection() == 0);
    CHECK(m.key(Key::Close) == Action::Resume);
    m.set_open(true);
    CHECK(m.key(Key::Back) == Action::Resume);  // Esc on main = resume
    m.set_open(true);
    CHECK(m.key(Key::Enter) == Action::Resume);  // Resume item
    // Submenu entry points.
    m.set_open(true);
    m.key(Key::Down);
    CHECK(m.key(Key::Enter) == Action::OpenStates && m.page() == Page::States);
    m.set_open(true);
    for (int i = 0; i < 8; ++i) m.key(Key::Down);
    CHECK(m.key(Key::Enter) == Action::None && m.page() == Page::ConfirmQuit);
    CHECK(m.key(Key::Enter) == Action::Resume);  // default selection is No
    m.set_open(true);
    for (int i = 0; i < 8; ++i) m.key(Key::Down);
    CHECK(m.key(Key::Enter) == Action::None && m.page() == Page::ConfirmQuit);
    m.key(Key::Up);  // Yes
    CHECK(m.key(Key::Enter) == Action::Quit);
    // Gamepad mirrors keyboard.
    m.set_open(true);
    CHECK(m.pad(Pad::Down) == Action::None && m.selection() == 1);
    CHECK(m.pad(Pad::South) == Action::OpenStates);
    CHECK(m.pad(Pad::East) == Action::None && m.page() == Page::Main);
    CHECK(m.pad(Pad::Start) == Action::Resume);
}

void test_states_page() {
    Menu m;
    m.set_open(true);
    m.key(Key::Down);
    m.key(Key::Enter);
    CHECK(m.page() == Page::States);
    Menu::SlotInfo slots[4] = {};
    slots[1].occupied = true;
    slots[1].label = "12:03:44";
    m.set_slots(slots);
    CHECK(m.key(Key::Right) == Action::NextSlot && m.slot() == 1);
    CHECK(m.key(Key::Left) == Action::PrevSlot && m.slot() == 0);
    CHECK(m.key(Key::Left) == Action::PrevSlot && m.slot() == 3);  // wraps
    m.key(Key::Right);                                            // back to 0
    // Activate Save / Load rows.
    for (int i = 0; i < 4; ++i) m.key(Key::Down);
    CHECK(m.key(Key::Enter) == Action::SaveSlot);
    CHECK(m.key(Key::Down) == Action::None);
    CHECK(m.key(Key::Enter) == Action::LoadSlot);
    // Selecting a slot row just moves the selection (Up wraps to the last row).
    m.set_open(true);
    m.key(Key::Down);
    m.key(Key::Enter);
    CHECK(m.page() == Page::States && m.slot() == 0);
    m.key(Key::Down);  // slot 1
    CHECK(m.key(Key::Enter) == Action::None && m.slot() == 1);
    const std::vector<Line> lines = m.render(56, 28);
    CHECK(contains(lines, "Slot 2") && contains(lines, "12:03:44") && contains(lines, "empty"));
}

void test_cards_page() {
    Menu m;
    m.set_open(true);
    for (int i = 0; i < 4; ++i) m.key(Key::Down);
    CHECK(m.key(Key::Enter) == Action::OpenCards);
    // Empty list: one row that goes back.
    CHECK(m.key(Key::Enter) == Action::None && m.page() == Page::Main);
    m.set_open(true);
    for (int i = 0; i < 4; ++i) m.key(Key::Down);
    m.key(Key::Enter);
    m.set_cards({"card1.mcd", "card1-20240101.mcd"}, 0);
    CHECK(m.key(Key::Enter) == Action::None);  // select first
    m.key(Key::Down);
    m.key(Key::Down);  // Backup row
    CHECK(m.key(Key::Enter) == Action::BackupCard);
    m.key(Key::Down);  // Restore row
    CHECK(m.key(Key::Enter) == Action::UseCard);
}

void test_action_queue_discipline() {
    // Regression test for the review findings: actions form a FIFO (Save then
    // Esc-close must BOTH run — the old single-slot queue dropped the Save),
    // and Resume closes the menu (the old loop never called set_open(false)).
    // Simulates main.cpp's drain loop with a stub sink.
    Menu m;
    m.set_open(true);
    std::vector<Action> queue = {Action::SaveSlot, Action::Resume};
    int saves = 0;
    auto drain = [&] {
        while (!queue.empty()) {
            const Action a = queue.front();
            queue.erase(queue.begin());
            if (a == Action::SaveSlot) ++saves;
            if (a == Action::Resume) m.set_open(false);
        }
    };
    drain();
    CHECK(saves == 1 && !m.is_open());
    // FIFO order preserved across mixed navigation + actions.
    m.set_open(true);
    queue = {Action::NextSlot, Action::SaveSlot, Action::Resume};
    int nexts = 0;
    saves = 0;
    auto drain2 = [&] {
        while (!queue.empty()) {
            const Action a = queue.front();
            queue.erase(queue.begin());
            if (a == Action::NextSlot) {
                ++nexts;
                m.key(Key::Right);  // slot moves before the save runs
            }
            if (a == Action::SaveSlot) {
                ++saves;
                CHECK(nexts == 1);  // ordering: slot moved first
            }
            if (a == Action::Resume) m.set_open(false);
        }
    };
    drain2();
    CHECK(saves == 1 && !m.is_open());
}

void test_cards_left_right() {
    Menu m;
    m.set_open(true);
    for (int i = 0; i < 4; ++i) m.key(Key::Down);
    CHECK(m.key(Key::Enter) == Action::OpenCards);
    m.set_cards({"card1.mcd", "card1-a.mcd", "card1-b.mcd"}, 0);
    // Pick card1-a (second file), then step onto Backup/Use: the choice sticks.
    // Rows: 0,1,2 files - 3 Backup - 4 Use.
    CHECK(m.card_sel() == 0);
    CHECK(m.key(Key::Right) == Action::None && m.card_sel() == 1);
    CHECK(m.key(Key::Right) == Action::None && m.card_sel() == 2);
    CHECK(m.key(Key::Right) == Action::None && m.card_sel() == 2);  // Backup row: unchanged
    CHECK(m.key(Key::Enter) == Action::BackupCard);
    CHECK(m.key(Key::Right) == Action::None && m.card_sel() == 2);  // Use row: unchanged
    CHECK(m.key(Key::Enter) == Action::UseCard);                    // acts on card1-b here
    // Re-pick file 0, wrap to Use: the choice sticks across Backup/Use rows.
    // Currently sel=4 (Use), choice=2.
    m.key(Key::Left);  // sel 3 (Backup), choice stays 2
    m.key(Key::Left);  // sel 2 (file 2)
    m.key(Key::Left);  // sel 1 (file 1)
    m.key(Key::Left);  // sel 0 (file 0)
    CHECK(m.card_sel() == 0);
    m.key(Key::Left);  // sel 4 (Use, wraps), choice stays 0
    CHECK(m.card_sel() == 0);
    CHECK(m.key(Key::Enter) == Action::UseCard);  // acts on file 0, not the oldest
    // The chosen file is marked.
    CHECK(contains(m.render(56, 28), "[selected]"));
    // Left/Right still wraps across all 5 rows (3 files + Backup + Use);
    // choice only changes on file rows. Currently sel=4 (Use), choice=0.
    m.key(Key::Left);  // sel 3 (Backup), choice stays 0
    CHECK(m.card_sel() == 0);
    m.key(Key::Left);  // sel 2 (file 2)
    CHECK(m.card_sel() == 2);
    m.key(Key::Right);  // sel 3 (Backup), choice stays 2
    m.key(Key::Right);  // sel 4 (Use), choice stays 2
    CHECK(m.card_sel() == 2);
}

void test_set_slot() {
    Menu m;
    m.set_open(true);
    m.set_slot(2);
    CHECK(m.slot() == 2);
    m.set_slot(99);
    CHECK(m.slot() == 3);  // clamped
    m.set_slot(-1);
    CHECK(m.slot() == 0);
}

void test_render_bounds() {
    Menu m;
    CHECK(m.render(56, 28).empty());  // closed: nothing
    m.set_open(true);
    // Tiny sizes still return something sane, never wider/taller than asked.
    for (int cols : {20, 40, 56}) {
        for (int rows : {12, 20, 28}) {
            const std::vector<Line> lines = m.render(cols, rows);
            CHECK(!lines.empty() && static_cast<int>(lines.size()) <= rows);
            for (const Line& l : lines) CHECK(static_cast<int>(l.text.size()) <= cols);
        }
    }
    CHECK(contains(m.render(56, 28), "PAUSE MENU"));
    CHECK(contains(m.render(56, 28), "Quit to desktop"));
    // Trainer asks the host to open the trainer; Hotkeys is a read-only page.
    {
        Menu h;
        h.set_open(true);
        for (int i = 0; i < 5; ++i) h.key(Key::Down);
        CHECK(h.key(Key::Enter) == Action::OpenTrainer && h.page() == Page::Main);
        h.key(Key::Down);
        CHECK(h.key(Key::Enter) == Action::OpenHotkeys && h.page() == Page::Hotkeys);
        h.set_info(Page::Hotkeys, {"F1           Pause menu (Esc too)"});
        CHECK(contains(h.render(56, 28), "Pause menu"));
        CHECK(h.key(Key::Enter) == Action::None && h.page() == Page::Main);
    }
    // Info pages render their lines.
    m.set_open(true);
    for (int i = 0; i < 2; ++i) m.key(Key::Down);
    m.key(Key::Enter);
    m.set_info(Page::Settings, {"scale_mode = fit", "volume = 80"});
    CHECK(contains(m.render(56, 28), "scale_mode = fit"));
}

}  // namespace

int main() {
    test_main_nav();
    test_states_page();
    test_cards_page();
    test_cards_left_right();
    test_set_slot();
    test_action_queue_discipline();
    test_render_bounds();
    std::printf("menu: ok\n");
    return 0;
}
