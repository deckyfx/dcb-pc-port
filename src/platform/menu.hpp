#pragma once
// Native pause menu: the item tree, navigation state and actions, with no SDL
// in it. The backend (sdl3_menu.*) routes keys/gamepad to key()/gamepad(),
// draws render() lines, and executes the actions main.cpp hands in through
// the callbacks. Unit-tested without a display, like trainer.*.
//
// The game is frozen while the menu is open (the host loop skips
// resume_guest(), the same way the trainer freezes it), so opening/closing
// the menu never changes guest state or guest time.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace menu {

// Panel keys (the backend maps its keyboard/gamepad to these).
enum class Key : uint8_t { Up, Down, Left, Right, Enter, Back, Close };
// Gamepad buttons the menu understands (positional names, like settings.ini).
enum class Pad : uint8_t { Up, Down, Left, Right, South, East, Start };

// How a menu line is drawn (mirrors trainer::Style so backends share code).
enum class Style : uint8_t { Normal, Title, Selected, Dim, Good, Error };

struct Line {
    std::string text;
    Style style = Style::Normal;
};

/// Panel size the backend should aim for (in 8x8 characters); render() adapts
/// to less, like the trainer panel.
inline constexpr int kMenuCols = 56;
inline constexpr int kMenuRows = 28;

/// What the menu can do. The backend reports user intent; main.cpp performs
/// it (save states, settings writes, card copies, quit) at the frame boundary.
enum class Action : uint8_t {
    None,
    Resume,            ///< close the menu, resume the game
    OpenStates,        ///< submenu: save/load slots
    OpenSettings,      ///< submenu: display + audio
    OpenControls,      ///< submenu: bindings + hotkeys (read-only)
    OpenCards,         ///< submenu: memory-card backup/restore
    OpenAbout,         ///< submenu: version/build/credits
    OpenTrainer,       ///< close the menu and open the trainer panel
    OpenHotkeys,       ///< submenu: every hotkey and what it does (read-only)
    Quit,              ///< quit (backend asks for confirmation first)
    SaveSlot,          ///< save into the selected slot (with thumbnail)
    LoadSlot,          ///< load from the selected slot
    PrevSlot,          ///< select the previous slot (wraps)
    NextSlot,          ///< select the next slot (wraps)
    CycleResolution,   ///< settings submenu: advance the window resolution (1x/2x/4x/8x)
    CycleDisplay,      ///< settings submenu: advance the focused display option
    CycleAudio,        ///< settings submenu: advance volume
    ToggleFullscreen,  ///< settings submenu: flip fullscreen now
    BackupCard,        ///< copy card1.mcd to a timestamped backup
    UseCard,           ///< make the selected file the live card (copy over card1.mcd + reload)
    PrevCard,          ///< select the previous card file/backup (wraps)
    NextCard,          ///< select the next card file/backup (wraps)
};

/// Which page is shown.
enum class Page : uint8_t { Main, States, Settings, Controls, Cards, About, Hotkeys, ConfirmQuit };

class Menu {
public:
    Menu();

    bool is_open() const { return open_; }
    void set_open(bool open);
    /// Adopt an externally selected slot (the F6 hotkey while the menu is
    /// closed). Clamped to 0-3.
    void set_slot(int slot) { slot_ = std::clamp(slot, 0, 3); }

    /// A navigation / action key. Returns the action for main.cpp (None if the
    /// key only moved the selection).
    Action key(Key k);
    /// A gamepad button. Returns the action for main.cpp.
    Action pad(Pad b);

    Page page() const { return page_; }
    int selection() const { return sel_; }
    int slot() const { return slot_; }        ///< selected state slot, 0-based
    int card_sel() const { return card_sel_; }  ///< selected card file, 0-based

    /// Slot metadata for the States page (set by main.cpp from SaveStates).
    struct SlotInfo {
        bool occupied = false;
        std::string label;  ///< e.g. "Slot 2 - 12:03:44" or "Slot 2 - empty"
        int thumb_w = 0, thumb_h = 0;
        std::vector<uint8_t> thumb_rgb;  ///< thumb_w*thumb_h*3, 8-bit RGB (may be empty)
    };
    void set_slots(const SlotInfo (&slots)[4]) {
        for (int i = 0; i < 4; ++i) slots_[i] = slots[i];
    }

    /// Card-file list for the Cards page (set by main.cpp).
    void set_cards(std::vector<std::string> names, int active);

    /// Text lines for the read-only pages (set by main.cpp: settings values,
    /// bindings, version/build/credits). Shown verbatim under the title.
    void set_info(Page page, std::vector<std::string> lines);

    /// The menu as at most `rows` lines of at most `cols` characters.
    std::vector<Line> render(int cols, int rows) const;

    /// Thumbnail for the selected States row (backend draws it next to the
    /// row). False when the slot has none.
    bool selected_thumbnail(int& w, int& h, const uint8_t*& rgb) const;

    /// Quit-confirmation state (backend draws "Quit? Enter=yes Esc=no").
    bool confirming_quit() const { return page_ == Page::ConfirmQuit; }

private:
    int item_count() const;
    Action activate(int item);
    void render_main(std::vector<Line>& out) const;
    void render_states(std::vector<Line>& out) const;
    void render_info(std::vector<Line>& out, const char* title) const;

    bool open_ = false;
    Page page_ = Page::Main;
    int sel_ = 0;
    int slot_ = 0;
    int card_sel_ = 0;
    SlotInfo slots_[4];
    std::vector<std::string> cards_;
    int card_active_ = 0;
    std::vector<std::string> info_settings_;
    std::vector<std::string> info_controls_;
    std::vector<std::string> info_about_;
    std::vector<std::string> info_hotkeys_;
    mutable int scroll_ = 0;  ///< first visible row (render keeps selection visible)
};

}  // namespace menu
