// New cards in the game (src/patch/cards.hpp; docs/re/cards.md, "Where 301 cards are assumed").
//
// A new card's record sits at Digimon index 191 + k: once B:\CARD2.CDD is loaded, the database
// is rebuilt in port RAM with the new records after the 191 Digimon and the Digimon count raised
// (800481D8 card_db_load, below; the file itself is served unchanged so the heap does not move), so
// code that works by deck index or record address already finds it. The other overrides here are
// the functions that work by card number and decide the kind by the ranges 0-190 / 191-292 /
// 293-300, where 301 + k would read as a Digivolve card:
//   80048A8C card_record_get(n)          the record
//   800489DC card_specialty_icon(n)      record +0x1A high nibble
//   80048A34 card_level_icon(n)          record +0x1A low nibble
//   80048988 card_from_kind_index(k, i)  Digimon index 191 + k -> card 301 + k
//   800495D8 deck_entry_set(entry, n)    card 301 + k -> kind 0, index 191 + k
//   8004979C deck_store(p, deck, slot)   rebuilds each entry's card number from kind + index
//                                        inline (a Digimon's number is its index): the new
//                                        cards' numbers put back
//   KAWSEG 801EDD88 preview_slot()       the battle's card picture cache slot, by a card number
//                                        made the same way: native, with the right number
//   80030578 digimon_record_by_model(m)  searches the new records too
//   8004835C card_copy_serial(p, n, i)   none for new cards: the serials (u16 x 6 per card, read
//                                        only by the trade) of card 301 would land in the city flags
//   80048D68 battle_rewards_pick(p, c)   native: the same pools over every card, new ones included
//                                        (the original treats numbers above 292 as Digivolve
//                                        records and sizes its lists for 301)
// and two battle functions that count per Digimon index into arrays of 191 (best damage, KOs
// scored / taken), where index 191 + k would land in the next array (or the collection):
//   KAWSEG 801ED064 best damage          skipped for a new card
//   KAWSEG 801EF968 battle_ko_check      the original, with the bytes a new card's counts would
//                                        overwrite put back
//
// The collection keeps one byte per card at game_data+0x1482 (301 bytes), followed by one pad
// byte, +0x15AF: card 301's byte. More new cards need a collection outside game_data (the save
// file's unused tail), so only the first is used for now.

#include "cards.hpp"

#include "patch/card_art.hpp"
#include "patch/cards.hpp"

#include "../overrides/native_files.hpp"
#include "vfs/image.hpp"
#include "gpu/gpu.hpp"
#include "gpu/hd_textures.hpp"
#include "hw/mmio.hpp"

#include <psx/backtrace.hpp>

#include <psx/recomp.h>
#include <psx/runtime.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>

extern "C" {
void f_800484AC(PsxContext* ctx);  // collection_clear_new()
void f_8006BBB0(PsxContext* ctx);  // rand() -> 0..0x7FFF
}
#include <string>
#include <vector>

namespace {

namespace pc = patch::cards;
namespace art = patch::card_art;

constexpr int kA0 = 4, kA1 = 5, kA2 = 6, kV0 = 2;
constexpr int kOptions = 0x125 - pc::kDigimon;  // 191-292
constexpr int kOldCards = 301;
constexpr size_t kMaxNew = 1;  // the collection's pad byte (see above)

constexpr uint32_t kDigimonBase = 0x801DB000u;  // -> Digimon records (0x134 each)
constexpr uint32_t kOptionBase = 0x801DAFF8u, kDigivolveBase = 0x801DAFFCu;  // -> Option / Digivolve records
constexpr uint32_t kDatabaseBuffer = 0x801DB004u;  // -> the loaded CARD2.CDD
constexpr uint32_t kOptionRecord = 0xDA, kDigivolveRecord = 0x68;
constexpr uint32_t kGameData = 0x80070C2Cu;     // -> game_data, 0x2738 bytes per player
constexpr uint32_t kGameDataSize = 0x2738u;
constexpr uint32_t kBattleState = 0x801DAF38u;   // -> the battle state
constexpr uint32_t kBattlePlayers = 0x801DAF40u;  // -> battle player structs, one per player
constexpr uint32_t kSlots = 0x1C6;                // player: 3 bytes, the slots' cards (-1: none)
constexpr uint32_t kHand = 0x10, kHandEntry = 8;  // player: entries {kind, index, u16 card, record}
constexpr uint32_t kKosScored = 0x1186, kKosTaken = 0x1304;

constexpr uint32_t kBestDamage = 0x801ED064u, kKoCheck = 0x801EF968u;

// Rewards (docs/re/cards.md, "Fusion and rewards").
constexpr uint32_t kRewardClasses = 0x80070F38u;  // 16 rows x 0x12: [specialty * 3 + level column] Digimon bounds,
constexpr uint32_t kClassSize = 0x12;             // then +0xF / +0x10 Option bounds, +0x11 the rare margin
constexpr uint32_t kPartners = 0x2F8, kPartnerSize = 0x288, kPartnerRare = 0x304 - 0x2F8;
constexpr uint32_t kRewards = 0x272C;  // u16 x 3: the reward (normal or rare) and two normal ones

}  // namespace

extern "C" {
/// The card count for the bounds the recompiler takes from it (config/<id>/immediates.json):
/// 301 + the new cards once B:\CARD2.CDD has their records, else 301.
int32_t dcb_card_limit = 301;
/// The Card Menu's "all copies" mark (1608 for the 301 cards): 6 more per new card.
int32_t dcb_card_copies_limit = 1608;
}

namespace {

struct State {
    bool loaded = false;
    std::vector<pc::Card> cards;
    std::vector<uint8_t> spec_levels;  ///< record +0x1A of each Digimon (0-190), once the database is loaded
    std::map<int, std::optional<art::Image>> pictures;  ///< by card: the art's square picture (64x64)
};

State& state() {
    static State s;
    if (!s.loaded) {
        s.loaded = true;
        std::string why;
        s.cards = pc::load_builtin(&why);
        if (!why.empty()) std::fprintf(stderr, "[cards] %s", why.c_str());
        if (s.cards.size() > kMaxNew) {
            std::fprintf(stderr, "[cards] only %zu new card(s) can be kept in the collection for now; cards from %d on left out\n",
                         kMaxNew, pc::kFirstNew + static_cast<int>(kMaxNew));
            s.cards.resize(kMaxNew);
        }
    }
    return s;
}

/// The Digimon index of a new card number, or -1.
int new_index(int card) { return pc::index_of(card, dcb::cards::count()); }

uint32_t digimon_record(PsxContext& ctx, int index) {
    return psx_read32(&ctx, kDigimonBase) + static_cast<uint32_t>(index) * pc::kDigimonRecord;
}

/// The Digimon index (deck entry +1) of the card in player `p`'s active slot, or -1 (battle).
int active_index(PsxContext& ctx, uint32_t p) {
    const uint32_t player = psx_read32(&ctx, kBattlePlayers + 4 * p);
    for (uint32_t i = 0; i < 3; ++i) {
        const int8_t slot = static_cast<int8_t>(psx_read8(&ctx, player + kSlots + i));
        if (slot != -1) return psx_read8(&ctx, player + kHand + static_cast<uint32_t>(slot % 30) * kHandEntry + 1);
    }
    return -1;
}

/// battle_rewards_pick over cards 0 .. limit - 1 (see dcb_battle_rewards_pick).
void rewards_pick(PsxContext* ctx, int limit) {
    const uint32_t player = ctx->r[kA0];
    const int32_t cls = static_cast<int32_t>(ctx->r[kA1]);
    f_800484AC(ctx);
    const uint32_t data = psx_read32(ctx, kGameData) + player * kGameDataSize;
    const uint32_t row = kRewardClasses + static_cast<uint32_t>(cls) * kClassSize;
    const auto bound = [&](uint32_t at) { return static_cast<int>(static_cast<int8_t>(psx_read8(ctx, row + at))); };
    int rare_chance = 0;
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t partner = data + kPartners + i * kPartnerSize;
        if (psx_read8(ctx, partner) != 0) rare_chance += psx_read8(ctx, partner + kPartnerRare);
    }
    std::vector<uint16_t> normal, rare;
    for (int n = 0; n < limit; ++n) {
        const uint32_t rec = dcb::cards::record(*ctx, n);
        const uint8_t kind = psx_read8(ctx, rec + 2), spec_level = psx_read8(ctx, rec + 0x1A);
        int hi = 0, lo = 0;
        if (kind == 0) {
            const int level = spec_level & 0xF, column = level == 2 ? 1 : level == 0 ? 2 : 0;
            hi = bound(static_cast<uint32_t>(column + (spec_level >> 4) * 3));
            if (hi < 0) {
                lo = -hi;
                hi = 99;
            }
        } else if (kind < 3) {
            hi = bound(0xF);
            lo = bound(0x10);
        }
        const int r = psx_read8(ctx, rec + 0x19);
        if (r == 0) continue;
        if (r <= hi && r >= lo) normal.push_back(static_cast<uint16_t>(n));
        else if (hi != 0 && r <= hi + bound(0x11) && r >= lo) rare.push_back(static_cast<uint16_t>(n));
    }
    const auto roll = [&] {
        f_8006BBB0(ctx);
        return static_cast<int32_t>(ctx->r[kV0]);
    };
    const auto pick = [&](const std::vector<uint16_t>& pool) {
        const int32_t v = roll();
        return pool.empty() ? uint16_t{0} : pool[static_cast<size_t>(v % static_cast<int32_t>(pool.size()))];
    };
    const bool rare_roll = roll() % 100 < rare_chance;
    psx_write16(ctx, data + kRewards, pick(rare_roll && !rare.empty() ? rare : normal));
    psx_write16(ctx, data + kRewards + 2, pick(normal));
    psx_write16(ctx, data + kRewards + 4, pick(normal));
}

/// DCB_VERIFY_REWARDS=1: the original and the native pick over the original 301 cards, from the
/// same RAM, must agree: the three cards, rand()'s seed and the collection bytes (the original's
/// heap buffers leave other bytes behind). Logged, then the native one with the new cards runs.
void verify_rewards(PsxContext* ctx) {
    constexpr uint32_t kRandSeed = 0x801E0808u;
    const std::vector<uint8_t> ram(ctx->ram, ctx->ram + PSX_RAM_SIZE);
    const PsxContext regs = *ctx;
    const uint32_t data = psx_read32(ctx, kGameData) + regs.r[kA0] * kGameDataSize;
    struct Result {
        uint16_t cards[3];
        uint32_t seed;
        std::vector<uint8_t> collection;
    };
    const auto result = [&] {
        Result r{};
        for (uint32_t i = 0; i < 3; ++i) r.cards[i] = psx_read16(ctx, data + kRewards + 2 * i);
        r.seed = psx_read32(ctx, kRandSeed);
        for (uint32_t i = 0; i < kOldCards + 1; ++i) r.collection.push_back(psx_read8(ctx, data + 0x1482 + i));
        return r;
    };
    psx_call_original(ctx, 0x80048D68u);
    const Result want = result();
    const std::vector<uint8_t> after(ctx->ram, ctx->ram + PSX_RAM_SIZE);
    std::copy(ram.begin(), ram.end(), ctx->ram);
    *ctx = regs;
    rewards_pick(ctx, kOldCards);
    const Result got = result();
    size_t diffs = 0, first = 0, last = 0;
    for (size_t i = 0; i < PSX_RAM_SIZE; ++i)
        if (ctx->ram[i] != after[i]) {
            if (diffs++ == 0) first = i;
            last = i;
        }
    const bool same = std::equal(want.cards, want.cards + 3, got.cards) && want.seed == got.seed && want.collection == got.collection;
    std::printf("[cards] rewards check: original %u %u %u, native %u %u %u, seed %s, collection %s: %s "
                "(other RAM: %zu bytes differ in %08zX-%08zX)\n",
                want.cards[0], want.cards[1], want.cards[2], got.cards[0], got.cards[1], got.cards[2],
                want.seed == got.seed ? "same" : "DIFFERENT", want.collection == got.collection ? "same" : "DIFFERENT",
                same ? "ok" : "MISMATCH", diffs, 0x80000000u + first, 0x80000000u + last);
    std::copy(ram.begin(), ram.end(), ctx->ram);
    *ctx = regs;
}

}  // namespace

namespace dcb::cards {

size_t count() { return state().cards.size(); }

int card_limit() { return kOldCards + static_cast<int>(count()); }

int base_of(int card) {
    const int i = new_index(card);
    return i < 0 ? -1 : state().cards[static_cast<size_t>(i - pc::kDigimon)].base;
}

namespace {

/// The new card whose battle model is served for `key` ("F/225.PAK") as a copy, or nullptr.
/// The new card for B/CARD/LC<number>.TIM, or nullptr.
const pc::Card* card_picture(const std::string& key) {
    for (const pc::Card& c : state().cards) {
        char name[24];
        std::snprintf(name, sizeof name, "B/CARD/LC%03d.TIM", c.number);
        if (key == name) return &c;
    }
    return nullptr;
}

/// The card's art as its 64x64 picture (assets/<serial>/<art>), loaded once; nullopt without art.
const std::optional<art::Image>& loaded_picture(const pc::Card& c) {
    auto& cache = state().pictures;
    if (const auto it = cache.find(c.number); it != cache.end()) return it->second;
    std::optional<art::Image>& out = cache[c.number];
    const std::string path = c.art.empty() ? std::string{} : dcb::asset_path(c.art);
    if (path.empty()) {
        if (!c.art.empty()) std::printf("[cards] %s: no %s under assets/: its base card's art stands in\n", c.name.c_str(), c.art.c_str());
        return out;
    }
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> file{std::istreambuf_iterator<char>(in), {}};
    art::Image img;
    if (!vfs::decode_rgba(file.data(), file.size(), img.w, img.h, img.rgba)) {
        std::fprintf(stderr, "[cards] %s: cannot read %s as an image (PNG, JPEG, BMP, ...)\n", c.name.c_str(), path.c_str());
        return out;
    }
    out = art::picture(img, 64);
    if (out) std::printf("[cards] %s: art from %s (%dx%d)\n", c.name.c_str(), path.c_str(), img.w, img.h);
    return out;
}

/// The US version of a JP mini card, as the texture replacer uploads it (the English art: the
/// group's US palette, the US badge), or nullopt without one.
std::optional<art::Tim8> us_version(const art::Tim8& jp) {
    PsxContext* ctx = psx::active_context();
    auto* mmio = ctx ? static_cast<hle::Mmio*>(psx::Machine::from(ctx).mmio()) : nullptr;
    hle::HdTextures* hd = mmio ? mmio->gpu().hd() : nullptr;
    if (!hd || !hd->enabled()) return std::nullopt;
    std::vector<uint32_t> words(128);
    for (size_t i = 0; i < 128; ++i) words[i] = jp.palette[2 * i] | static_cast<uint32_t>(jp.palette[2 * i + 1]) << 16;
    const std::vector<uint16_t>* pal = hd->maybe_replace(jp.clut_x, jp.clut_y, 256, 1, words.data(), words.size());
    if (!pal || pal->size() < 256) return std::nullopt;
    art::Tim8 us = jp;
    std::copy(pal->begin(), pal->begin() + 256, us.palette.begin());
    const int hw = jp.w / 2;  // halfwords across
    words.assign((static_cast<size_t>(hw) * static_cast<size_t>(jp.h) + 1) / 2, 0);
    for (size_t i = 0; i < jp.pixels.size(); ++i) words[i / 4] |= static_cast<uint32_t>(jp.pixels[i]) << (8 * (i % 4));
    const std::vector<uint16_t>* px = hd->maybe_replace(jp.x, jp.y, hw, jp.h, words.data(), words.size());
    if (!px || px->size() < static_cast<size_t>(hw) * static_cast<size_t>(jp.h)) return std::nullopt;
    for (size_t i = 0; i < jp.pixels.size(); ++i) us.pixels[i] = static_cast<uint8_t>((*px)[i / 2] >> (8 * (i % 2)));
    return us;
}

/// The mini card of each new card with art, for B/M_CARD.ARC (empty: a copy of its base's).
std::vector<patch::Bytes> minis(patch::View arc) {
    std::vector<patch::Bytes> out;
    const auto entry = [&](int n) -> std::optional<art::Tim8> {
        const size_t at = static_cast<size_t>(n) * 4;
        if (at + 8 > arc.size()) return std::nullopt;
        const uint32_t a = patch::rd32(arc, at), b = patch::rd32(arc, at + 4);
        if (a > b || b > arc.size()) return std::nullopt;
        return art::read_tim8(arc.subspan(a, b - a));
    };
    for (const pc::Card& c : state().cards) {
        out.emplace_back();
        const auto& pic = loaded_picture(c);
        if (!pic) continue;
        // The group's cards of the same level: their shared palette, and the badge they all have.
        const auto want = static_cast<uint8_t>(c.specialty << 4 | c.level);
        std::vector<art::Tim8> same;
        for (size_t n = 0; n < std::min<size_t>(172, state().spec_levels.size()); ++n)
            if (state().spec_levels[n] == want)
                if (auto t = entry(static_cast<int>(n))) same.push_back(std::move(*t));
        // With the English art the game draws the US versions (palette and badge): the new card is
        // made in those, and keeps the JP palette bytes so its palette upload is swapped the same way.
        std::vector<art::Tim8> us;
        for (const art::Tim8& t : same)
            if (auto u = us_version(t)) us.push_back(std::move(*u));
        const bool english = !same.empty() && us.size() == same.size();
        const art::Consensus common = art::common_pixels(english ? us : same, 60);
        if (common.keep.empty()) continue;
        art::Tim8 mini = art::shared_palette(common.like, art::resize(*pic, same[0].w, same[0].h), common.keep);
        mini.palette = same[0].palette;
        out.back() = art::write_tim8(mini);
        std::printf("[cards] %s: mini card from its art (%s palette and badge, from %zu cards)\n", c.name.c_str(),
                    english ? "US" : "JP", same.size());
    }
    return out;
}

const pc::Card* model_copy(const std::string& key) {
    for (const pc::Card& c : state().cards) {
        char name[16];
        std::snprintf(name, sizeof name, "F/%03d.PAK", c.model);
        if (c.model_copy_of >= 0 && key == name) return &c;
    }
    return nullptr;
}

}  // namespace

std::optional<std::vector<uint8_t>> picture_rgba(int card, int size) {
    const int i = new_index(card);
    if (i < 0) return std::nullopt;
    const auto& pic = loaded_picture(state().cards[static_cast<size_t>(i - pc::kDigimon)]);
    if (!pic) return std::nullopt;
    return art::resize(*pic, size, size).rgba;
}

bool wants(const std::string& key) {
    if (count() == 0) return false;
    if (key == "B/M_CARD.ARC" || model_copy(key) != nullptr) return true;
    const pc::Card* c = card_picture(key);
    return c && loaded_picture(*c).has_value();
}

bool patch_file(const std::string& key, std::vector<uint8_t>& bytes) {
    if (!wants(key)) return false;
    std::string why;
    if (const pc::Card* c = card_picture(key)) {  // the base's picture, served for it: the card's own
        const auto like = art::read_tim8(bytes);
        const auto& pic = loaded_picture(*c);
        if (!like || !pic) return false;
        bytes = art::write_tim8(art::own_palette(*like, *pic));
        std::printf("[cards] %s: %s's picture from its art\n", key.c_str(), c->name.c_str());
        return true;
    }
    const pc::Card* model = model_copy(key);
    // A model the player supplied already has its own id: re-tagging the copied id changes nothing.
    std::optional<patch::Bytes> out = model ? pc::copy_model(bytes, model->model_copy_of, model->model, &why)
                                            : pc::extend_mini_cards(bytes, state().cards, minis(bytes), &why);
    if (!out) {
        std::fprintf(stderr, "[cards] %s: not changed: %s\n", key.c_str(), why.c_str());
        return false;
    }
    if (model) std::printf("[cards] %s: model %d's entries re-tagged %d\n", key.c_str(), model->model_copy_of, model->model);
    else std::printf("[cards] %s: %zu new card(s) from %d\n", key.c_str(), count(), pc::kFirstNew);
    bytes = std::move(*out);
    return true;
}

std::string alias(const std::string& key) {
    char name[24];
    for (const pc::Card& c : state().cards) {
        // The battle model: F/<model>.PAK -> F/<model_copy_of>.PAK.
        std::snprintf(name, sizeof name, "F/%03d.PAK", c.model);
        if (c.model_copy_of >= 0 && key == name) {
            std::snprintf(name, sizeof name, "%03d.PAK", c.model_copy_of);
            return name;
        }
        // The card picture, until the card's own art is made: the base card's.
        std::snprintf(name, sizeof name, "B/CARD/LC%03d.TIM", c.number);
        if (key == name) {
            std::snprintf(name, sizeof name, "LC%03d.TIM", c.base);
            return name;
        }
    }
    return {};
}

uint32_t record(PsxContext& ctx, int card) {
    if (const int i = new_index(card); i >= 0) return digimon_record(ctx, i);
    if (card < 0 || card >= kOldCards) return 0;
    ctx.r[kA0] = static_cast<uint32_t>(card);
    psx_call_original(&ctx, 0x80048A8Cu);
    return ctx.r[kV0];
}

}  // namespace dcb::cards

extern "C" {

// 800481D8: card_db_load(): loads B:\\CARD2.CDD into a heap buffer the size of the file and sets
// the record bases (801DB000 Digimon, 801DAFF8 Options, 801DAFFC Digivolve) from its header counts,
// numbering the records. The file is served as it is, so the heap (and game_data after it, which
// cheats and the port address directly) stays where it was; then the database with the new cards'
// records after the 191 Digimon (header count raised) is built in port RAM (PSX_EXT_BASE) and the
// bases point there. The heap buffer stays allocated, unused.
void dcb_card_db_load(PsxContext* ctx) {
    psx_call_original(ctx, 0x800481D8u);
    if (dcb::cards::count() == 0) return;
    const uint32_t buffer = psx_read32(ctx, kDatabaseBuffer);
    const uint32_t digimon = psx_read16(ctx, buffer + 4), options = psx_read8(ctx, buffer + 6), digivolve = psx_read8(ctx, buffer + 7);
    constexpr uint32_t kHeader = static_cast<uint32_t>(pc::kHeader), kRecord = pc::kDigimonRecord;
    const uint32_t size = kHeader + digimon * kRecord + options * kOptionRecord + digivolve * kDigivolveRecord;
    patch::Bytes cdd(size);
    for (uint32_t i = 0; i < size; ++i) cdd[i] = psx_read8(ctx, buffer + i);
    state().spec_levels.clear();
    for (uint32_t n = 0; n < digimon; ++n) state().spec_levels.push_back(cdd[kHeader + n * kRecord + 0x1A]);
    std::string why;
    const std::optional<patch::Bytes> out = pc::extend_database(cdd, state().cards, &why);
    if (!out || out->size() > PSX_EXT_SIZE) {
        std::fprintf(stderr, "[cards] B:\\CARD2.CDD not extended: %s; the new cards are off\n", out ? "too big" : why.c_str());
        state().cards.clear();
        return;
    }
    std::memcpy(psx::Machine::from(ctx).ext(), out->data(), out->size());
    const uint32_t base = 0x80000000u | PSX_EXT_BASE, total = digimon + static_cast<uint32_t>(dcb::cards::count());
    psx_write32(ctx, kDigimonBase, base + kHeader);
    psx_write32(ctx, kOptionBase, base + kHeader + total * kRecord);
    psx_write32(ctx, kDigivolveBase, base + kHeader + total * kRecord + options * kOptionRecord);
    dcb_card_limit = dcb::cards::card_limit();
    dcb_card_copies_limit = 1608 + 6 * static_cast<int32_t>(dcb::cards::count());
    std::printf("[cards] database: %zu new card(s) from %d, in port RAM at %08X\n", dcb::cards::count(), pc::kFirstNew, base);
}

// 80048A8C: card_record_get(n) -> record.
void dcb_card_record_get(PsxContext* ctx) {
    if (const int i = new_index(static_cast<int32_t>(ctx->r[kA0])); i >= 0) {
        ctx->r[kV0] = digimon_record(*ctx, i);
        return;
    }
    psx_call_original(ctx, 0x80048A8Cu);
}

// 800489DC: card_specialty_icon(n) -> 0-4 specialty, 5 Option, 6 Digivolve.
void dcb_card_specialty_icon(PsxContext* ctx) {
    if (const int i = new_index(static_cast<int32_t>(ctx->r[kA0])); i >= 0) {
        ctx->r[kV0] = psx_read8(ctx, digimon_record(*ctx, i) + 0x1A) >> 4;
        return;
    }
    psx_call_original(ctx, 0x800489DCu);
}

// 80048A34: card_level_icon(n) -> 0-3 level, 4 Option, 5 Digivolve.
void dcb_card_level_icon(PsxContext* ctx) {
    if (const int i = new_index(static_cast<int32_t>(ctx->r[kA0])); i >= 0) {
        ctx->r[kV0] = psx_read8(ctx, digimon_record(*ctx, i) + 0x1A) & 0xF;
        return;
    }
    psx_call_original(ctx, 0x80048A34u);
}

// 80048988: card_from_kind_index(kind, index) -> card number (-1 for a bad kind).
void dcb_card_from_kind_index(PsxContext* ctx) {
    const int32_t kind = static_cast<int32_t>(ctx->r[kA0]), index = static_cast<int32_t>(ctx->r[kA1]);
    if (kind == 0 && index >= pc::kDigimon && index < pc::kDigimon + static_cast<int>(dcb::cards::count())) {
        ctx->r[kV0] = static_cast<uint32_t>(pc::kFirstNew + index - pc::kDigimon);
        return;
    }
    psx_call_original(ctx, 0x80048988u);
}

// 800495D8: deck_entry_set(entry, n): entry = {u8 kind, u8 index, u16 card}.
void dcb_deck_entry_set(PsxContext* ctx) {
    const uint32_t entry = ctx->r[kA0];
    const int card = static_cast<int32_t>(ctx->r[kA1]);
    if (const int i = new_index(card); i >= 0) {
        psx_write8(ctx, entry, 0);
        psx_write8(ctx, entry + 1, static_cast<uint8_t>(i));
        psx_write16(ctx, entry + 2, static_cast<uint16_t>(card));
        return;
    }
    psx_call_original(ctx, 0x800495D8u);
}

// 8004979C: deck_store(player, deck*, slot) -> 0, or -1 when there is no free slot: copies a deck
// into game_data (+0x2408 + slot * 0x10C; entries {kind, index, u16 card, record*} at +0x10 + 8 * i)
// and rebuilds each entry's card number and record from kind + index.
void dcb_deck_store(PsxContext* ctx) {
    const uint32_t player = ctx->r[kA0];
    psx_call_original(ctx, 0x8004979Cu);
    const uint32_t data = psx_read32(ctx, kGameData) + player * kGameDataSize;
    for (uint32_t deck = 0; deck < 3; ++deck)
        for (uint32_t i = 0; i < 30; ++i) {
            const uint32_t entry = data + 0x2408 + deck * 0x10C + 0x10 + i * 8;
            if (psx_read8(ctx, entry) != 0) continue;
            const int index = psx_read8(ctx, entry + 1);
            if (index >= pc::kDigimon && index < pc::kDigimon + static_cast<int>(dcb::cards::count()))
                psx_write16(ctx, entry + 2, static_cast<uint16_t>(pc::kFirstNew + index - pc::kDigimon));
        }
}

// KAWSEG 801EDD88: preview_slot(): which of the 6 cached card pictures (battle state +0x7E0: {s16
// card, u8 shown, u8 age}) the card under the cursor uses: its own, else a free one, else the
// oldest; into battle state +0x826.
void dcb_preview_slot(PsxContext* ctx) {
    const uint32_t state = psx_read32(ctx, kBattleState);
    const uint32_t player = psx_read32(ctx, kBattlePlayers + 4u * psx_read8(ctx, state + 0x81B));
    const int slot = static_cast<int16_t>(psx_read16(ctx, psx_read32(ctx, state + 0x58) + 2)) % 30;
    const uint32_t entry = player + 0x10 + static_cast<uint32_t>(slot) * 8;
    ctx->r[kA0] = psx_read8(ctx, entry);
    ctx->r[kA1] = psx_read8(ctx, entry + 1);
    dcb_card_from_kind_index(ctx);
    const int card = static_cast<int32_t>(ctx->r[kV0]);
    const auto cache = [&](uint32_t i) { return state + 0x7E0 + 4 * i; };
    uint32_t pick = 6;
    for (uint32_t i = 0; i < 6 && pick == 6; ++i)
        if (static_cast<int16_t>(psx_read16(ctx, cache(i))) == card) pick = i;
    if (pick == 6)
        for (uint32_t i = 0; i < 6 && pick == 6; ++i)
            if (static_cast<int16_t>(psx_read16(ctx, cache(i))) == -1) {
                psx_write8(ctx, cache(i) + 2, 0);
                pick = i;
            }
    if (pick == 6) {
        uint8_t oldest = 100;
        for (uint32_t i = 0; i < 6; ++i) oldest = std::min(oldest, psx_read8(ctx, cache(i) + 3));
        for (uint32_t i = 0; i < 6 && pick == 6; ++i)
            if (psx_read8(ctx, cache(i) + 3) == oldest) pick = i;
        if (pick == 6) pick = (psx_read8(ctx, state + 0x826) + 1u) % 6;
    }
    psx_write8(ctx, state + 0x826, static_cast<uint8_t>(pick));
}

// 80030578: digimon_record_by_model(model) -> the first Digimon record with that model (+0xE5),
// else the record past the last Digimon.
void dcb_digimon_record_by_model(PsxContext* ctx) {
    const uint8_t model = static_cast<uint8_t>(ctx->r[kA0]);
    const int total = pc::kDigimon + static_cast<int>(dcb::cards::count());
    int i = 0;
    while (i < total && psx_read8(ctx, digimon_record(*ctx, i) + 0xE5) != model) ++i;
    ctx->r[kV0] = digimon_record(*ctx, i);
}

// 8004835C: card_copy_serial(player, card, copy): a random serial for that copy.
void dcb_card_copy_serial(PsxContext* ctx) {
    if (static_cast<int32_t>(ctx->r[kA1]) >= kOldCards) return;
    psx_call_original(ctx, 0x8004835Cu);
}

// 80048D68: battle_rewards_pick(player, class): the three cards a won battle gives. Each card with
// a rarity (+0x19) goes into the normal pool when the class's bounds for its kind (Digimon: by
// specialty and level; a negative bound -b means b..99) hold it, into the rare pool when it is
// above them by at most the class's margin. Then: rare with the partners' rare bytes summed as
// the percentage, else normal; two more normal. The same rand() calls as the original, in order.
void dcb_battle_rewards_pick(PsxContext* ctx) {
    static const bool verify = std::getenv("DCB_VERIFY_REWARDS") != nullptr;
    if (verify) verify_rewards(ctx);
    rewards_pick(ctx, dcb::cards::card_limit());
}

// KAWSEG 801ED064: best_damage_update(player struct): the attack's damage into the per-Digimon
// best (game_data +0xD0C, u16 x 3 per Digimon index).
void dcb_best_damage_update(PsxContext* ctx) {
    const uint32_t p = psx_read32(ctx, ctx->r[kA0] + 0x174) >> 17 & 3;
    if (p <= 1 && active_index(*ctx, p) >= pc::kDigimon) return;
    psx_call_original(ctx, kBestDamage);
}

// KAWSEG 801EF968: battle_ko_check(player) -> 1 when its Digimon is KO'd. Counts a KO for each
// side's Digimon index (scored: winner's +0x1186, taken: loser's +0x1304, u16 per index); a new
// card's index writes past its array, so those two bytes are put back.
void dcb_battle_ko_check(PsxContext* ctx) {
    const uint32_t loser = ctx->r[kA0] & 1, winner = loser ^ 1;
    struct Saved {
        uint32_t addr = 0;
        uint16_t value = 0;
    } saved[2];
    const uint32_t data = psx_read32(ctx, kGameData);
    const auto keep = [&](Saved& s, uint32_t p, uint32_t array) {
        const int i = active_index(*ctx, p);
        if (i < pc::kDigimon) return;
        s.addr = data + p * kGameDataSize + array + static_cast<uint32_t>(i) * 2;
        s.value = psx_read16(ctx, s.addr);
    };
    keep(saved[0], winner, kKosScored);
    keep(saved[1], loser, kKosTaken);
    psx_call_original(ctx, kKoCheck);
    for (const Saved& s : saved)
        if (s.addr) psx_write16(ctx, s.addr, s.value);
}

}  // extern "C"
