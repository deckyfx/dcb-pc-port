#pragma once
// Trainer, part 2: memory search. Snapshots the 2 MB guest RAM and narrows a candidate list of
// addresses with value filters (equal / not equal / greater / less than a number) and change
// filters against the previous snapshot (changed / unchanged / increased / decreased). Classic
// "find the money counter" workflow: search = 500, spend some, search = 450, ... Pure: no SDL,
// unit-tested on plain buffers.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trainer {

/// Width of the searched value in bytes; addresses are aligned to it (as the game stores them).
enum class ValueSize : uint8_t { U8 = 1, U16 = 2, U32 = 4 };

enum class SearchFilter : uint8_t {
    Equal, NotEqual, Greater, Less,           ///< compare with a number
    Changed, Unchanged, Increased, Decreased,  ///< compare with the previous snapshot
};

inline constexpr size_t kSearchFilterCount = 8;

/// Short label ("= value", "changed", ...).
const char* filter_name(SearchFilter f);
/// Whether the filter compares against a typed number (vs the previous snapshot).
bool filter_needs_value(SearchFilter f);

/// Little-endian value of `size` at RAM offset `offset` (wraps inside the 2 MB buffer).
uint32_t read_value(const uint8_t* ram, uint32_t offset, ValueSize size);
/// Store `value` (truncated to `size`) at RAM offset `offset`.
void write_value(uint8_t* ram, uint32_t offset, ValueSize size, uint32_t value);

/// Parse a number typed by the user for a value of `size`: decimal (a leading '-' allowed:
/// stored as two's complement), or hex with a "0x" or "$" prefix. nullopt if malformed or out
/// of range for the size.
std::optional<uint32_t> parse_value(std::string_view text, ValueSize size);

/// Decimal text of a value (signed: sign-extended from `size`).
std::string format_value(uint32_t value, ValueSize size, bool is_signed);

class MemorySearch {
public:
    /// New search: every aligned address of `size` is a candidate; `ram` becomes the snapshot.
    void start(const uint8_t* ram, ValueSize size);
    /// Keep the candidates whose current value in `ram` passes `filter` (`value` for the
    /// value filters, the snapshot for the change filters), then take a new snapshot.
    /// Starts a search first if none is active. Returns the number of candidates left.
    size_t filter(const uint8_t* ram, SearchFilter filter, uint32_t value = 0);
    /// Forget the search (frees the snapshot).
    void reset();

    bool active() const { return !snapshot_.empty(); }
    ValueSize size() const { return size_; }
    /// Compare (and order) values as signed numbers.
    bool is_signed() const { return signed_; }
    void set_signed(bool s) { signed_ = s; }

    size_t count() const { return candidates_.size(); }
    /// RAM offset of candidate `i` (i < count()).
    uint32_t offset(size_t i) const { return candidates_[i]; }
    /// Value of candidate `i` in the last snapshot.
    uint32_t previous(size_t i) const { return read_value(snapshot_.data(), candidates_[i], size_); }

private:
    bool passes(uint32_t now, uint32_t before, SearchFilter f, uint32_t value) const;
    int64_t as_number(uint32_t v) const;

    ValueSize size_ = ValueSize::U16;
    bool signed_ = false;
    std::vector<uint8_t> snapshot_;
    std::vector<uint32_t> candidates_;
};

}  // namespace trainer
