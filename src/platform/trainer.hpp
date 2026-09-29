#pragma once
// Trainer: the cheat file, the memory search and the state of the in-window panel, tied to the
// guest RAM. The host loop calls apply_frame() at every frame boundary; the backend forwards
// keys / typed text while the panel is open and draws the lines render() returns. No SDL here:
// the panel logic is unit-tested with a plain RAM buffer.
//
// Guest RAM may only be touched while the game is suspended (docs/HOST_MAIN_LOOP.md). The host
// loop pumps events and calls apply_frame() between two resume_guest() calls, so everything
// here runs at the frame boundary.

#include "trainer_battle.hpp"
#include "trainer_cheats.hpp"
#include "trainer_search.hpp"
#include "trainer_toggles.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trainer {

/// Panel keys (the backend maps its keyboard to these; printable characters go to text()).
enum class Key : uint8_t { Up, Down, Left, Right, PageUp, PageDown, Home, End, Enter, Backspace, Delete, Tab, Close };

/// How a panel line is drawn.
enum class Style : uint8_t { Normal, Title, Selected, Dim, Good, Error };

struct Line {
    std::string text;
    Style style = Style::Normal;
};

/// Panel size the backend should aim for (in 8x8 characters); render() adapts to less.
inline constexpr int kPanelCols = 64;
inline constexpr int kPanelRows = 30;
/// Search results that can be listed and selected (the count is always shown).
inline constexpr size_t kMaxListedResults = 500;

class Trainer {
public:
    /// `ram`: the guest's 2 MB main RAM. `cheat_path`: the cheat file (need not exist yet).
    Trainer(uint8_t* ram, std::filesystem::path cheat_path);

    /// (Re)load the cheat file, discarding unsaved edits. A missing file is an empty list.
    /// Returns human-readable lines for the log (what was loaded, warnings).
    std::vector<std::string> load();
    /// Write the cheat file (creating cheats/). False on failure (status() says why).
    bool save();

    /// The port's built-in cheats for this game (GameShark text, like the cheat file), shown on the
    /// Presets tab. They cannot be edited or removed; their on/off state is kept in the cheat file
    /// as "!preset <name> on|off" lines. Call before or after load(); either order keeps the state.
    void set_presets(std::string_view text);
    const CheatSet& presets() const { return presets_; }

    /// Apply the enabled cheats (presets and the file's); call once per frame while the game is
    /// suspended.
    ApplyStats apply_frame();
    /// Log every frame's cheat writes to stderr (DCB_TRACE_CHEATS).
    void set_trace(bool on) { trace_ = on; }

    bool is_open() const { return open_; }
    void set_open(bool open);

    /// A navigation / action key while the panel is open.
    void key(Key k);
    /// Typed characters while the panel is open (UTF-8; only ASCII is used).
    void text(std::string_view chars);

    /// The panel as at most `rows` lines of at most `cols` characters (cols >= 20, rows >= 12
    /// for a usable panel; smaller sizes still return something sane).
    std::vector<Line> render(int cols, int rows) const;

    const CheatSet& cheats() const { return cheats_; }
    /// The Battle tab's actions (the battle hotkeys read them).
    const BattleActions& battle() const { return battle_; }
    /// The game toggles listed under them (Fusion Shop, progression flags; src/game reads them).
    const GameToggles& toggles() const { return toggles_; }
    const MemorySearch& search() const { return search_; }
    const std::filesystem::path& cheat_path() const { return path_; }
    const std::string& status() const { return status_; }
    bool dirty() const { return dirty_; }

private:
    enum class Tab : uint8_t { Presets, Cheats, Battle, Search };
    /// Rows of the Search tab before the result list.
    enum SearchRow : int { kRowSize, kRowSigned, kRowValue, kRowFilter, kRowNew, kSearchControls };

    void cheats_key(Key k);
    void presets_key(Key k);
    void presets_char(char c);
    void apply_preset_states();
    void battle_key(Key k);
    void battle_char(char c);
    void battle_step(int delta);
    void search_key(Key k);
    void cheats_char(char c);
    void search_char(char c);
    void move(int& sel, int count, Key k) const;
    int search_rows() const;
    void apply_filter();
    void freeze_selected();
    void write_selected();
    void set_status(std::string text, bool error = false);
    ValueSize size() const { return kSizes[size_index_]; }

    static constexpr ValueSize kSizes[3] = {ValueSize::U8, ValueSize::U16, ValueSize::U32};

    uint8_t* ram_;
    std::filesystem::path path_;
    CheatSet cheats_;
    CheatSet presets_;
    std::vector<std::pair<std::string, bool>> preset_states_;  ///< "!preset" lines from the file
    BattleActions battle_;
    GameToggles toggles_;
    MemorySearch search_;
    bool open_ = false;
    bool dirty_ = false;
    bool trace_ = false;
    uint64_t frame_ = 0;
    Tab tab_ = Tab::Battle;  ///< the first tab; Presets once the game supplies some
    int cheat_sel_ = 0;
    int preset_sel_ = 0;
    int battle_sel_ = 0;  ///< a battle action, then (past their count) a game toggle
    std::string battle_edit_;  ///< digits typed on the Battle tab, applied with Enter
    int search_sel_ = kRowValue;
    int size_index_ = 1;  ///< 16-bit
    int filter_index_ = 0;
    std::string value_text_;
    std::string status_;
    bool status_error_ = false;
    mutable int cheat_scroll_ = 0;   ///< first listed cheat (render keeps the selection visible)
    mutable int result_scroll_ = 0;  ///< first listed result
};

/// The game's trainer: cheat file from resolve_cheat_path() (DCB_CHEATS, ./cheats/<serial>.txt,
/// <exe dir>/cheats/<serial>.txt), loaded and logged to stdout; DCB_TRACE_CHEATS=1 logs every
/// frame's cheat writes.
std::unique_ptr<Trainer> make_trainer(uint8_t* ram, std::string_view serial);

}  // namespace trainer
