#pragma once
// Memory-card file management for the native menu: list card files, back up
// the active card, restore a backup. Pure filesystem logic (no SDL, no game
// objects); unit-tested. main.cpp performs the swap through Bios.
//
// Safety rule: the menu only runs while the game is frozen at a frame
// boundary. Card writes complete synchronously inside guest code
// (CardFs: "Everything completes synchronously"), so no write can be in
// flight while the menu is open. Backup copies the file bytes (MemoryCard
// persists on every write, so the file is always current). Restore swaps in
// a freshly loaded card object, which closes the game's open fds on that
// slot — the game sees it exactly like a physical card swap.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace memcard {

/// One card file in the save directory.
struct CardInfo {
    std::string name;  ///< file name ("card1.mcd", "card1-20240101-120000.mcd")
    uint64_t bytes = 0;
    bool is_backup = false;  ///< name starts with "card1-" (vs the live "card1.mcd")
};

/// Card files in `save_dir`, live card first, then backups newest-first.
std::vector<CardInfo> list_cards(const std::filesystem::path& save_dir);

/// Copy the live card to a timestamped backup
/// (`card1-YYYYMMDD-HHMMSS.mcd`). Returns the backup name, or "" on failure.
std::string backup_card(const std::filesystem::path& save_dir);

/// Make `name` (any .mcd in `save_dir`, live or backup) the live card:
/// auto-backup the current live card, then atomically copy over card1.mcd
/// (temp file + rename). Returns an error message, or empty on success. The
/// caller then swaps the card object (Bios reload), which closes the game's
/// open fds — like a physical swap.
std::string use_card(const std::filesystem::path& save_dir, const std::string& name);

}  // namespace memcard
