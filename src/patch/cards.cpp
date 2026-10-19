#include "patch/cards.hpp"

#include "patch/embed.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

namespace patch::cards {

namespace {

// Digimon record fields (docs/re/cards.md).
constexpr size_t kName = 3, kNameLen = 21;
constexpr size_t kFusion = 0x18, kRarity = 0x19, kSpecialtyLevel = 0x1A, kDp = 0x1B, kPlusP = 0x1C, kHp = 0x1E;
constexpr size_t kAttack = 0x20, kAttackSize = 28, kAttackName = 6, kAttackNameLen = 22;
constexpr size_t kSupport = 0x74, kSupportEnd = 0xE4;  // 2 conditions + 3 effects
constexpr size_t kCrossEffect = 0xE4, kModel = 0xE5, kText = 0xE7, kTextLen = 4 * 19;
constexpr size_t kOptionRecord = 0xDA, kDigivolveRecord = 0x68;

const char* const kSpecialties[] = {"fire", "ice", "nature", "darkness", "rare"};
const char* const kLevels[] = {"rookie", "armor", "champion", "ultimate"};
const char* const kCrossEffects[] = {"none",          "first_attack",     "circle_to_0",   "triangle_to_0",
                                     "cross_to_0",    "counter_circle",   "counter_triangle", "counter_cross",
                                     "crash",         "eat_up_hp",        "jamming",       "x3_fire",
                                     "x3_ice",        "x3_nature",        "x3_darkness",   "x3_rare"};
const char* const kAttacks[] = {"circle", "triangle", "cross"};

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

template <size_t N>
int lookup(const char* const (&names)[N], const std::string& v) {
    for (size_t i = 0; i < N; ++i)
        if (v == names[i]) return static_cast<int>(i);
    return -1;
}

bool number(const std::string& v, int& out) {
    if (v.empty()) return false;
    size_t i = v[0] == '-' ? 1 : 0;
    if (i == v.size()) return false;
    for (size_t k = i; k < v.size(); ++k)
        if (!std::isdigit(static_cast<unsigned char>(v[k]))) return false;
    out = std::stoi(v);
    return true;
}

bool fail(std::string* why, const std::string& msg) {
    if (why) *why = msg;
    return false;
}

void put_string(Bytes& b, size_t at, const std::string& s, size_t len) {
    std::fill(b.begin() + static_cast<long>(at), b.begin() + static_cast<long>(at + len), 0);
    std::memcpy(b.data() + at, s.data(), std::min(s.size(), len - 1));
}

void put16(Bytes& b, size_t at, int v) {
    b[at] = static_cast<uint8_t>(v);
    b[at + 1] = static_cast<uint8_t>(v >> 8);
}

void put32(Bytes& b, size_t at, uint32_t v) {
    for (size_t i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

}  // namespace

std::optional<Card> parse(const std::string& text, std::string* why) {
    Card c;
    std::string section;
    std::map<std::string, bool> seen;
    size_t start = 0;
    int line_no = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        ++line_no;
        if (const size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        line = trim(line);
        if (line.empty()) {
            if (end == text.size()) break;
            continue;
        }
        const std::string at = "line " + std::to_string(line_no) + ": ";
        if (line.front() == '[') {
            if (line.back() != ']') return fail(why, at + "bad section"), std::nullopt;
            section = trim(line.substr(1, line.size() - 2));
            if (section != "card" && !(section.rfind("attack ", 0) == 0 && lookup(kAttacks, trim(section.substr(7))) >= 0))
                return fail(why, at + "unknown section [" + section + "]"), std::nullopt;
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) return fail(why, at + "expected key = value"), std::nullopt;
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        seen[section + "." + key] = true;
        int n = 0;
        if (section.rfind("attack ", 0) == 0) {
            Attack& a = c.attacks[static_cast<size_t>(lookup(kAttacks, trim(section.substr(7))))];
            if (key == "name" && !value.empty() && value.size() < kAttackNameLen) a.name = value;
            else if (key == "power" && number(value, n) && n >= 0 && n <= 9990) a.power = n;
            else return fail(why, at + "bad " + key), std::nullopt;
            continue;
        }
        if (section != "card") return fail(why, at + "key outside a section"), std::nullopt;
        const auto num = [&](int& field, int lo, int hi) {
            if (!number(value, n) || n < lo || n > hi) return false;
            field = n;
            return true;
        };
        bool ok = true;
        if (key == "number") ok = num(c.number, kFirstNew, kFirstNew + 64);
        else if (key == "base") ok = num(c.base, 0, kDigimon - 1);
        else if (key == "name") ok = !value.empty() && value.size() < kNameLen && (c.name = value, true);
        else if (key == "specialty") ok = (c.specialty = lookup(kSpecialties, value)) >= 0;
        else if (key == "level") ok = (c.level = lookup(kLevels, value)) >= 0;
        else if (key == "hp") ok = num(c.hp, 10, 9990);
        else if (key == "dp") ok = num(c.dp, 0, 90);
        else if (key == "plus_p") ok = num(c.plus_p, 0, 90);
        else if (key == "fusion") ok = num(c.fusion, 0, 35);
        else if (key == "rarity") ok = num(c.rarity, 0, 99);
        else if (key == "cross_effect") ok = (c.cross_effect = lookup(kCrossEffects, value)) >= 0;
        else if (key == "support_from") ok = num(c.support_from, 0, kDigimon - 1);
        else if (key == "model") ok = num(c.model, 0, 255);  // a byte in the record
        else if (key == "model_copy_of") ok = num(c.model_copy_of, 0, 255);
        else if (key == "art") ok = !value.empty() && (c.art = value, true);
        else return fail(why, at + "unknown key " + key), std::nullopt;
        if (!ok) return fail(why, at + "bad " + key + " = " + value), std::nullopt;
    }
    for (const char* k : {"card.number", "card.base", "card.name", "card.specialty", "card.level", "card.hp", "card.model"})
        if (!seen[k]) return fail(why, std::string("missing ") + k), std::nullopt;
    for (size_t i = 0; i < 3; ++i)
        if (!seen[std::string("attack ") + kAttacks[i] + ".name"])
            return fail(why, std::string("missing [attack ") + kAttacks[i] + "] name"), std::nullopt;
    return c;
}

std::vector<Card> load_builtin(std::string* why) {
    std::vector<Card> cards;
    std::string problems;
    for (const embedded::File& f : embedded::cards_config()) {
        std::string w;
        if (std::optional<Card> c = parse(std::string(f.text()), &w)) cards.push_back(std::move(*c));
        else problems += std::string(f.name) + ": " + w + "\n";
    }
    std::sort(cards.begin(), cards.end(), [](const Card& a, const Card& b) { return a.number < b.number; });
    for (size_t i = 0; i < cards.size(); ++i) {
        if (cards[i].number != kFirstNew + static_cast<int>(i)) {
            problems += "card " + std::to_string(cards[i].number) + ": the numbers must follow on from " +
                        std::to_string(kFirstNew) + " without a gap\n";
            cards.resize(i);
            break;
        }
    }
    if (why) *why = problems;
    return cards;
}

std::optional<Bytes> extend_database(View cdd, const std::vector<Card>& cards, std::string* why) {
    if (cdd.size() < kHeader || std::memcmp(cdd.data(), "ADCD", 4) != 0) return fail(why, "not a CARD2.CDD"), std::nullopt;
    const size_t digimon = rd16(cdd, 4), options = cdd[6], digivolve = cdd[7];
    const size_t end_digimon = kHeader + digimon * kDigimonRecord;
    if (digimon != kDigimon || cdd.size() != end_digimon + options * kOptionRecord + digivolve * kDigivolveRecord)
        return fail(why, "unexpected CARD2.CDD layout"), std::nullopt;
    Bytes out(cdd.begin(), cdd.begin() + static_cast<long>(end_digimon));
    for (const Card& c : cards) {
        if (c.base < 0 || c.base >= kDigimon || (c.support_from >= kDigimon)) return fail(why, c.name + ": bad base or support"), std::nullopt;
        const auto record = [&](int n) { return cdd.subspan(kHeader + static_cast<size_t>(n) * kDigimonRecord, kDigimonRecord); };
        Bytes r(record(c.base).begin(), record(c.base).end());
        put16(r, 0, c.number);  // the loader writes the numbers of its own 191; this one is ours
        r[2] = 0;               // Digimon
        put_string(r, kName, c.name, kNameLen);
        r[kFusion] = static_cast<uint8_t>(c.fusion);
        r[kRarity] = static_cast<uint8_t>(c.rarity);
        r[kSpecialtyLevel] = static_cast<uint8_t>(c.specialty << 4 | c.level);
        r[kDp] = static_cast<uint8_t>(c.dp);
        r[kPlusP] = static_cast<uint8_t>(c.plus_p);
        put16(r, kHp, c.hp);
        for (size_t i = 0; i < 3; ++i) {
            const size_t a = kAttack + i * kAttackSize;
            put16(r, a, c.attacks[i].power);
            put_string(r, a + kAttackName, c.attacks[i].name, kAttackNameLen);
        }
        if (c.support_from >= 0) {
            const View s = record(c.support_from);
            std::copy(s.begin() + kSupport, s.begin() + kSupportEnd, r.begin() + kSupport);
            std::copy(s.begin() + kText, s.begin() + kText + kTextLen, r.begin() + kText);
        }
        r[kCrossEffect] = static_cast<uint8_t>(c.cross_effect);
        r[kModel] = static_cast<uint8_t>(c.model);
        out.insert(out.end(), r.begin(), r.end());
    }
    out.insert(out.end(), cdd.begin() + static_cast<long>(end_digimon), cdd.end());
    put16(out, 4, static_cast<int>(digimon + cards.size()));
    return out;
}

std::optional<Bytes> copy_model(View pak, int from, int to, std::string* why) {
    Bytes out(pak.begin(), pak.end());
    size_t at = 0;
    for (;;) {
        // The end is a negative type (the game reads only that; the disc's files end in 4 bytes).
        if (at + 2 > out.size()) return fail(why, "not a model PAK (no end entry)"), std::nullopt;
        const auto type = static_cast<int16_t>(rd16(out, at));
        if (type < 0) return out;
        if (at + 8 > out.size()) return fail(why, "not a model PAK (an entry past the end)"), std::nullopt;
        if (static_cast<int16_t>(rd16(out, at + 2)) == from) put16(out, at + 2, to);
        const size_t size = rd32(out, at + 4);
        if (size > out.size() - at - 8) return fail(why, "not a model PAK (entry past the end)"), std::nullopt;
        at += 8 + size;
    }
}

std::optional<Bytes> extend_mini_cards(View arc, const std::vector<Card>& cards, const std::vector<Bytes>& minis,
                                       std::string* why) {
    constexpr size_t kOld = 301;  // entries
    if (arc.size() < (kOld + 1) * 4 || rd32(arc, 0) != (kOld + 1) * 4 || rd32(arc, kOld * 4) != arc.size())
        return fail(why, "unexpected M_CARD.ARC layout"), std::nullopt;
    const size_t added = cards.size();
    std::vector<View> entries;
    for (size_t i = 0; i < kOld; ++i) {
        const uint32_t a = rd32(arc, i * 4), b = rd32(arc, (i + 1) * 4);
        if (a > b || b > arc.size()) return fail(why, "bad M_CARD.ARC offset"), std::nullopt;
        entries.push_back(arc.subspan(a, b - a));
    }
    for (size_t i = 0; i < added; ++i)
        entries.push_back(i < minis.size() && !minis[i].empty() ? View(minis[i]) : entries[static_cast<size_t>(cards[i].base)]);
    Bytes out((entries.size() + 1) * 4);
    for (size_t i = 0; i < entries.size(); ++i) {
        put32(out, i * 4, static_cast<uint32_t>(out.size()));
        out.insert(out.end(), entries[i].begin(), entries[i].end());
    }
    put32(out, entries.size() * 4, static_cast<uint32_t>(out.size()));
    return out;
}

}  // namespace patch::cards
