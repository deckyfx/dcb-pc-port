#pragma once
// New cards in the game (definitions and database: src/patch/cards.hpp; docs/wiki/Cards.md):
// the files the game reads for them, and the overrides that map their numbers (301, ...) to the
// Digimon index their records sit at (191, ...).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct PsxContext;

namespace dcb::cards {

/// The new cards' count (0 when none are defined, or they could not be used).
size_t count();

/// True for a game file the cards change while there are new cards: B/M_CARD.ARC (their mini
/// cards) and a battle model served as a copy (F/<model>.PAK: the copied model's entries are
/// re-tagged with the new id, which the game looks them up by). Their records are not added to B/CARD2.CDD as it is read (a bigger file would move the
/// heap after it): the database is extended in port RAM once loaded (card_db_load, cards.cpp).
bool wants(const std::string& key);

/// A game file the cards change (see wants): patched in place; false when the file is not theirs.
bool patch_file(const std::string& key, std::vector<uint8_t>& bytes);

/// A file the disc does not have that stands for one it has: a new card's battle model
/// (F/<model>.PAK -> <model_copy_of>.PAK, when the disc has no F/<model>.PAK) and, until its art is made, its picture
/// (B/CARD/LC<number>.TIM -> LC<base>.TIM). The name of that file in the same folder, or "".
std::string alias(const std::string& key);

/// A card number's record, for numbers the game knows (0-300) and the new ones; 0 for others.
uint32_t record(PsxContext& ctx, int card);

/// One past the highest card number (301 + the new cards).
int card_limit();

/// A new card's picture from its art, `size` x `size` RGBA (row by row); nullopt without art.
std::optional<std::vector<uint8_t>> picture_rgba(int card, int size);

/// The card a new card is based on (its definition's `base`), or -1 for other numbers.
int base_of(int card);

}  // namespace dcb::cards
