#pragma once
// Wizardmon's spell as typed (keyword.cpp): the keywords and the completion codes, matched on
// text alone (tests/game/test_keyword_match.cpp).

#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>

namespace dcb::keyword {

// The keywords by index (the script's order: Omnimon's first card at 0), JP as typed in
// half-width, and the US release's.
inline constexpr std::array<const char*, 10> kJp = {"OMEGA1", "WARGRY", "OMEGAS", "MTLGRR", "AERO-V",
                                                    "H-KABU", "VENOMV", "PIEMON", "MTLETE", "JI2MON"};
inline constexpr std::array<const char*, 10> kUs = {"OMNIMON-1",   "WARGREYMON",   "OMNIMON-2",   "MTLGARURUMON",
                                                    "A-VEEDRAMON", "H-KBUTERIMON", "VENOMMYOTIS", "PIEDMON",
                                                    "MTLETEMON",   "JIJIMON"};

/// Upper case, no dashes or spaces; full-width letters, digits and dashes as ASCII.
inline std::string normalize(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<uint8_t>(s[i]);
        if (c >= 0x81 && i + 1 < s.size()) {
            const uint16_t w = static_cast<uint16_t>(c << 8 | static_cast<uint8_t>(s[i + 1]));
            ++i;
            if (w >= 0x8260 && w <= 0x8279) out.push_back(static_cast<char>('A' + (w - 0x8260)));
            else if (w >= 0x8281 && w <= 0x829A) out.push_back(static_cast<char>('A' + (w - 0x8281)));
            else if (w >= 0x824F && w <= 0x8258) out.push_back(static_cast<char>('0' + (w - 0x824F)));
            else if (w == 0x815B || w == 0x815C || w == 0x815D || w == 0x817C || w == 0x8140) continue;
            else out += '?';
            continue;
        }
        if (c == '-' || c == ' ') continue;
        out.push_back(static_cast<char>(std::toupper(c)));
    }
    return out;
}

inline std::optional<int> keyword_index(const std::string& typed) {
    for (size_t i = 0; i < kJp.size(); ++i)
        if (typed == normalize(kJp[i]) || typed == normalize(kUs[i])) return static_cast<int>(i);
    return std::nullopt;
}

/// `prefix` followed by 1-3 digits: the number.
inline std::optional<int> numbered(const std::string& typed, const std::string& prefix) {
    if (typed.size() <= prefix.size() || typed.size() > prefix.size() + 3 || typed.compare(0, prefix.size(), prefix) != 0)
        return std::nullopt;
    int n = 0;
    for (size_t i = prefix.size(); i < typed.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(typed[i]))) return std::nullopt;
        n = n * 10 + (typed[i] - '0');
    }
    return n;
}

}  // namespace dcb::keyword
