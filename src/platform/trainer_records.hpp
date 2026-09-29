#pragma once
// Deck record editor: the wins/losses of the three saved decks (the trainer's General tab,
// under the partner slots). Works on the guest's 2 MB main RAM while the game is suspended,
// like the partner editor's "write once"; nothing is kept in the cheat file (save in game to
// keep a change). No SDL, no guest runtime: unit-tested on a plain RAM buffer
// (tests/trainer).
//
// What the game keeps (docs/re/save-data.md, "Saved decks"):
//   - game_data + 0x2408 + deck * 0x10C: the deck record. +0x00 in use (0 = unused),
//     +0x104 / +0x106 / +0x108: u16 battles / wins / losses (capped at 999; the VS and
//     result screens show wins + losses as the battle count, then wins, then losses).
//   - After a battle the game's record update (caseD_0, 800422B4) adds one to the winner's
//     deck +0x106 and the loser's +0x108 (capped, 999 kept), and battle_counters_add
//     (KAWSEG 801FCF78) the per-opponent +0x818 counters. There is no separate
//     player-level total: what the screens show as "yours" is the active deck's record.
//
// set_record() writes wins/losses of one deck once (clamped 0-999, refused when no save is
// loaded or the deck is unused). The battle count is not stored: the game displays
// wins + losses.

#include <array>
#include <cstdint>
#include <string>

namespace trainer {

inline constexpr int kRecordDecks = 3;
inline constexpr int kRecordMax = 999;  ///< the game's cap (caseD_0 keeps 999)

/// Wins and losses of one deck as the game shows them.
struct DeckRecord {
    int wins = 0;
    int losses = 0;
    bool used = false;  ///< deck record in use (+0x00 != 0)
};

/// The three decks' records, or why they cannot be read (no save loaded).
struct RecordState {
    bool ok = false;
    std::string error;
    std::array<DeckRecord, kRecordDecks> decks{};
};

/// Reads the decks' records from `ram` (kRamSize bytes).
RecordState read_records(const uint8_t* ram);

struct RecordChange {
    bool ok = false;
    std::string message;  ///< what was done, or why nothing was
};

/// Writes `wins`/`losses` (clamped 0-999) to deck `deck` (0-2) once. Refused (nothing
/// written) when no save is loaded or the deck is unused.
RecordChange set_record(uint8_t* ram, int deck, int wins, int losses);

}  // namespace trainer
