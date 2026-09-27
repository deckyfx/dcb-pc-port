// Native pause menu logic (see menu.hpp).

#include "menu.hpp"

#include <algorithm>

namespace menu {

namespace {

const char* kMainItems[] = {"Resume",           "Save / Load state", "Settings",
                            "Controls",         "Memory card",       "About",
                            "Quit to desktop"};
constexpr int kMainCount = 7;

const char* kSlotNames[4] = {"Slot 1", "Slot 2", "Slot 3", "Slot 4"};

}  // namespace

Menu::Menu() = default;

void Menu::set_open(bool open) {
    open_ = open;
    if (open) {
        page_ = Page::Main;
        sel_ = 0;
        scroll_ = 0;
    }
}

void Menu::set_cards(std::vector<std::string> names, int active) {
    cards_ = std::move(names);
    card_active_ = std::clamp(active, 0, std::max(0, static_cast<int>(cards_.size()) - 1));
    card_sel_ = std::clamp(card_sel_, 0, std::max(0, static_cast<int>(cards_.size()) - 1));
}

void Menu::set_info(Page page, std::vector<std::string> lines) {
    switch (page) {
    case Page::Main: break;
    case Page::States: break;
    case Page::Settings: info_settings_ = std::move(lines); break;
    case Page::Controls: info_controls_ = std::move(lines); break;
    case Page::Cards: break;
    case Page::About: info_about_ = std::move(lines); break;
    case Page::ConfirmQuit: break;
    }
}

int Menu::item_count() const {
    switch (page_) {
    case Page::Main: return kMainCount;
    case Page::States: return 4 + 2;  // 4 slots + Save + Load rows act on selection
    case Page::Settings: return 5;    // resolution, scale mode, filter/aspect, volume, fullscreen
    case Page::Controls: return 1;    // read-only; any Enter goes back
    case Page::Cards: return cards_.empty() ? 1 : static_cast<int>(cards_.size()) + 2;
    case Page::About: return 1;
    case Page::ConfirmQuit: return 2;  // Yes / No
    }
    return 1;
}

Action Menu::activate(int item) {
    switch (page_) {
    case Page::Main:
        switch (item) {
        case 0: return Action::Resume;
        case 1: page_ = Page::States; sel_ = slot_; scroll_ = 0; return Action::OpenStates;
        case 2: page_ = Page::Settings; sel_ = 0; scroll_ = 0; return Action::OpenSettings;
        case 3: page_ = Page::Controls; sel_ = 0; scroll_ = 0; return Action::OpenControls;
        case 4: page_ = Page::Cards; sel_ = 0; scroll_ = 0; return Action::OpenCards;
        case 5: page_ = Page::About; sel_ = 0; scroll_ = 0; return Action::OpenAbout;
        case 6: page_ = Page::ConfirmQuit; sel_ = 1; scroll_ = 0; return Action::None;
        }
        break;
    case Page::States:
        if (item < 4) {
            slot_ = item;
            sel_ = item;
            return Action::None;
        }
        return item == 4 ? Action::SaveSlot : Action::LoadSlot;
    case Page::Settings:
        switch (item) {
        case 0: return Action::CycleResolution;
        case 1:
        case 2: return Action::CycleDisplay;
        case 3: return Action::CycleAudio;
        case 4: return Action::ToggleFullscreen;
        }
        break;
    case Page::Controls:
    case Page::About: page_ = Page::Main; sel_ = 0; scroll_ = 0; return Action::None;
    case Page::Cards:
        if (cards_.empty()) {
            page_ = Page::Main;
            sel_ = 0;
            scroll_ = 0;
            return Action::None;
        }
        if (item < static_cast<int>(cards_.size())) {
            card_sel_ = item;
            return Action::None;
        }
        return item == static_cast<int>(cards_.size()) ? Action::BackupCard : Action::UseCard;
    case Page::ConfirmQuit: return item == 0 ? Action::Quit : Action::Resume;
    }
    return Action::None;
}

Action Menu::key(Key k) {
    if (!open_) return Action::None;
    const int n = item_count();
    switch (k) {
    case Key::Up:
        sel_ = (sel_ + n - 1) % n;
        return Action::None;
    case Key::Down: sel_ = (sel_ + 1) % n; return Action::None;
    case Key::Left:
        if (page_ == Page::States) {
            slot_ = (slot_ + 3) % 4;
            sel_ = slot_;
            return Action::PrevSlot;
        }
        if (page_ == Page::Cards && !cards_.empty()) {
            // Move within files + Backup/Use rows (wraps). card_sel_ only
            // follows on file rows: stepping onto Backup/Use must not
            // reselect, or "Use selected file" would act on the last file.
            const int rows = static_cast<int>(cards_.size()) + 2;
            sel_ = (sel_ + rows - 1) % rows;
            if (sel_ < static_cast<int>(cards_.size())) card_sel_ = sel_;
            return Action::None;
        }
        return Action::None;
    case Key::Right:
        if (page_ == Page::States) {
            slot_ = (slot_ + 1) % 4;
            sel_ = slot_;
            return Action::NextSlot;
        }
        if (page_ == Page::Cards && !cards_.empty()) {
            const int rows = static_cast<int>(cards_.size()) + 2;
            sel_ = (sel_ + 1) % rows;
            if (sel_ < static_cast<int>(cards_.size())) card_sel_ = sel_;
            return Action::None;
        }
        return Action::None;
    case Key::Enter: return activate(sel_);
    case Key::Back:
        if (page_ == Page::ConfirmQuit) return Action::Resume;
        if (page_ != Page::Main) {
            page_ = Page::Main;
            sel_ = 0;
            scroll_ = 0;
            return Action::None;
        }
        return Action::Resume;
    case Key::Close: return Action::Resume;
    }
    return Action::None;
}

Action Menu::pad(Pad b) {
    if (!open_) return Action::None;
    switch (b) {
    case Pad::Up: return key(Key::Up);
    case Pad::Down: return key(Key::Down);
    case Pad::Left: return key(Key::Left);
    case Pad::Right: return key(Key::Right);
    case Pad::South: return key(Key::Enter);
    case Pad::East: return key(Key::Back);
    case Pad::Start: return key(Key::Close);
    }
    return Action::None;
}

void Menu::render_main(std::vector<Line>& out) const {
    out.push_back({"PAUSE MENU", Style::Title});
    out.push_back({"", Style::Normal});
    for (int i = 0; i < kMainCount; ++i)
        out.push_back({std::string(i == sel_ ? "> " : "  ") + kMainItems[i],
                       i == sel_ ? Style::Selected : Style::Normal});
    out.push_back({"", Style::Normal});
    out.push_back({"Arrows/Enter Esc=resume", Style::Dim});
}

/// Thumbnail for the selected States row, if the slot has one (the backend
/// draws it next to the row; the text render stays backend-agnostic).
bool Menu::selected_thumbnail(int& w, int& h, const uint8_t*& rgb) const {
    if (page_ != Page::States || sel_ < 0 || sel_ >= 4) return false;
    const SlotInfo& s = slots_[sel_];
    if (!s.occupied || s.thumb_w <= 0 || s.thumb_h <= 0 || s.thumb_rgb.empty()) return false;
    w = s.thumb_w;
    h = s.thumb_h;
    rgb = s.thumb_rgb.data();
    return true;
}

void Menu::render_states(std::vector<Line>& out) const {
    out.push_back({"SAVE / LOAD STATE", Style::Title});
    out.push_back({"", Style::Normal});
    for (int i = 0; i < 4; ++i) {
        std::string text = std::string(i == sel_ ? "> " : "  ") + kSlotNames[i] + ": ";
        text += slots_[i].occupied ? slots_[i].label : "empty";
        if (i == slot_) text += "  [active]";
        out.push_back({text, i == sel_ ? Style::Selected : Style::Normal});
    }
    out.push_back({std::string(sel_ == 4 ? "> " : "  ") + "Save into selected slot",
                   sel_ == 4 ? Style::Selected : Style::Normal});
    out.push_back({std::string(sel_ == 5 ? "> " : "  ") + "Load from selected slot",
                   sel_ == 5 ? Style::Selected : Style::Normal});
    out.push_back({"", Style::Normal});
    out.push_back({"Left/Right switches slot - Esc=back", Style::Dim});
}

void Menu::render_info(std::vector<Line>& out, const char* title) const {
    out.push_back({title, Style::Title});
    out.push_back({"", Style::Normal});
    const std::vector<std::string>* lines = nullptr;
    if (page_ == Page::Settings) lines = &info_settings_;
    else if (page_ == Page::Controls) lines = &info_controls_;
    else if (page_ == Page::About) lines = &info_about_;
    if (lines) {
        for (const std::string& l : *lines) out.push_back({l, Style::Normal});
    }
    if (page_ == Page::Settings) {
        static const char* kRows[5] = {"Resolution", "Scale mode", "Filter / aspect", "Volume",
                                       "Fullscreen: toggle"};
        out.push_back({"", Style::Normal});
        for (int i = 0; i < 5; ++i)
            out.push_back({std::string(i == sel_ ? "> " : "  ") + kRows[i],
                           i == sel_ ? Style::Selected : Style::Normal});
    }
    if (page_ == Page::Cards) {
        if (cards_.empty()) {
            out.push_back({"No card files found.", Style::Dim});
        } else {
            for (size_t i = 0; i < cards_.size(); ++i) {
                std::string text = std::string(static_cast<int>(i) == sel_ ? "> " : "  ") + cards_[i];
                if (static_cast<int>(i) == card_active_) text += "  [in use]";
                if (static_cast<int>(i) == card_sel_) text += "  [selected]";
                out.push_back({text, static_cast<int>(i) == sel_ ? Style::Selected : Style::Normal});
            }
            const int nb = static_cast<int>(cards_.size());
            out.push_back({std::string(sel_ == nb ? "> " : "  ") + "Back up active card",
                           sel_ == nb ? Style::Selected : Style::Normal});
            out.push_back({std::string(sel_ == nb + 1 ? "> " : "  ") + "Use selected file as card",
                           sel_ == nb + 1 ? Style::Selected : Style::Normal});
        }
    }
    if (page_ == Page::ConfirmQuit) {
        out.push_back({std::string(sel_ == 0 ? "> " : "  ") + "Yes, quit",
                       sel_ == 0 ? Style::Selected : Style::Error});
        out.push_back({std::string(sel_ == 1 ? "> " : "  ") + "No, keep playing",
                       sel_ == 1 ? Style::Selected : Style::Good});
    }
    out.push_back({"", Style::Normal});
    out.push_back({"Enter=select  Esc=back", Style::Dim});
}

std::vector<Line> Menu::render(int cols, int rows) const {
    std::vector<Line> all;
    if (!open_) return all;
    switch (page_) {
    case Page::Main: render_main(all); break;
    case Page::States: render_states(all); break;
    default: {
        const char* title = "MENU";
        if (page_ == Page::Settings) title = "SETTINGS";
        else if (page_ == Page::Controls) title = "CONTROLS";
        else if (page_ == Page::Cards) title = "MEMORY CARD";
        else if (page_ == Page::About) title = "ABOUT";
        else if (page_ == Page::ConfirmQuit) title = "QUIT?";
        render_info(all, title);
        break;
    }
    }
    // Clamp width, keep the selection visible vertically.
    for (Line& l : all) {
        if (static_cast<int>(l.text.size()) > cols) l.text.resize(static_cast<size_t>(cols));
    }
    if (static_cast<int>(all.size()) <= rows) {
        scroll_ = 0;
        return all;
    }
    int want = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        if (!all[i].text.empty() && all[i].text[0] == '>') want = static_cast<int>(i);
    }
    scroll_ = std::clamp(want - rows / 2, 0, static_cast<int>(all.size()) - rows);
    return std::vector<Line>(all.begin() + scroll_, all.begin() + scroll_ + rows);
}

}  // namespace menu
