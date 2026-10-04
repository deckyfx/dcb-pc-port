// The US art for the JP game: C++ port of tools/assets/swap_us_images.py. See art_swap.hpp.
//
// The port follows the script step for step (same names where it can), so that its result is
// the same upload for upload (tools/patch/compare_art.sh checks it against a Python run). Where
// the script would stop with an exception on malformed data, this skips the entry instead.

#include "patch/art_swap.hpp"

#include "vfs/hash.hpp"
#include "vfs/image.hpp"
#include "vfs/rip.hpp"
#include "vfs/tim.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>

namespace patch::art {

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

// ---------------------------------------------------------------------------------------------
// The tables (swap_us_images.py's; the reasons for each entry are in its docstring and comments).
// ---------------------------------------------------------------------------------------------

struct EntryGroup {
    const char* drv;
    const char* pattern;  ///< regex on the entry path, anchored at the end
};
// The same-geometry groups of docs/HYBRID_EN_ASSETS.md section 5 (DEFAULT_ENTRIES).
const EntryGroup kDefaultEntries[] = {
    {"A.DRV", R"(BATTLE\.PAK)"},
    {"B.DRV", R"((M_CARD|P_CARD|PARTNER|OPENING|FRIEND|TRADE|BCARD|CBTL_SYS)\.ARC)"},
    {"B.DRV", R"(CARD/LC\d+\.TIM)"},
    {"E.DRV", R"(\d+\.PAK)"},
    {"F.DRV", R"(\d+\.PAK)"},
};
// TIS image lists and the VS / result screens' ARCs, read from the disc (TIS_ENTRIES).
const EntryGroup kTisEntries[] = {
    {"C.DRV", R"(AREA\d\d\.PAK)"},
    {"C.DRV", R"(OBJECT/(WORLD|DECK|UNIT)\.TIS)"},
    {"B.DRV", R"((MATCH|WIN)/\d+\.ARC)"},
};

struct KeepRect {  ///< KEEP_JP: (DRV, VRAM image rect) -> why
    const char* drv;
    Rect image;
    const char* why;
};
const std::vector<KeepRect> kKeepJp = {};

struct KeepAt {  ///< KEEP_JP_AT: (DRV, x, y) -> why
    const char* drv;
    uint16_t x, y;
    const char* why;
};
const KeepAt kKeepJpAt[] = {
    {"B.DRV", 704, 480,
     "the VS screen's opponent name pictures (tools/text/bigfont.py grafts the US one into the MATCH archives; "
     "its palette is the big font's)"},
};

struct ComposeRule {  ///< COMPOSE: (DRV, VRAM image rect) -> blocks
    const char* drv;
    Rect image;
    std::vector<Block> blocks;
};
// The city HELP MENU plate: the US X icon moves one row down, the JP circle icon goes in its place.
const std::vector<ComposeRule> kCompose = {
    {"C.DRV", {808, 0, 22, 80}, {{false, 13, 26, 11, 11, 13, 41}, {true, 13, 26, 11, 11, 13, 26}}},
};

struct DrawScale {  ///< JP_DRAW_SCALE: (DRV, VRAM image rect) -> (texels read, pixels shown)
    const char* drv;
    Rect image;
    int read, shown;
};
const DrawScale kJpDrawScale[] = {
    {"C.DRV", {792, 0, 16, 144}, 68, 64},  // the city sub-menu labels
};

struct NarrowRule {  ///< NARROW: (DRV, JP image rect) -> (US image rect, texels kept from the right)
    const char* drv;
    Rect jp;
    Rect us;
    int right;
};
const NarrowRule kNarrow[] = {
    {"B.DRV", {464, 184, 34, 18}, {464, 184, 48, 18}, 10},  // the VS / result record strip
};

struct FitRule {  ///< FIT: (DRV, entry, JP image rect) -> (US image rect, slot width in pixels or 0)
    const char* drv;
    const char* entry;
    Rect jp;
    Rect us;
    int slot_w;
};
// US art whose layout differs, fitted into the JP image and its palette the way the texture
// replacer fits a PNG (fit_image): the title, which the pairing leaves alone (the US one has
// another logo and image list). The subtitle, US 320x48, goes into the JP 224x32 one (drawn
// smaller, config/SLPS-03101/sprites.txt); the US copyright, 256 wide, into the JP 176-wide one
// with a 256-wide slot (drawn 1:1 from it, like the US game); NEW GAME, CONTINUE and Battle
// with Friend have the JP size. The JP palettes stay (the logo and the D-1 Grand Prix label
// share them).
const FitRule kFit[] = {
    {"B.DRV", "TITLE.ARC", {704, 0, 112, 32}, {704, 0, 160, 48}, 0},
    {"B.DRV", "TITLE.ARC", {512, 168, 44, 32}, {512, 168, 64, 32}, 256},
    {"B.DRV", "TITLE.ARC", {704, 128, 24, 28}, {704, 128, 24, 28}, 0},
    {"B.DRV", "TITLE.ARC", {704, 156, 24, 28}, {704, 156, 24, 28}, 0},
    {"B.DRV", "TITLE.ARC", {704, 184, 24, 28}, {704, 184, 24, 28}, 0},
};

const char* const kNever = R"((^|/)SYSTEM\.TIM$)";
const char* const kPartial = R"(^B\.DRV:(MATCH|WIN)/\d+\.ARC$)";
const char* const kJpOnlyDrvs[] = {"W.DRV", "X.DRV", "Y.DRV", "Z.DRV"};  // D-1 Grand Prix

// ---------------------------------------------------------------------------------------------

constexpr uint64_t kSector = 0x800;

uint32_t load32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof v);
    return v;
}

uint16_t load16(const uint8_t* p) {
    uint16_t v = 0;
    std::memcpy(&v, p, sizeof v);
    return v;
}

/// data[a:b] with Python's slice rules (clamped; empty when b <= a).
Bytes slice(const Bytes& data, uint64_t a, uint64_t b) {
    a = std::min<uint64_t>(a, data.size());
    b = std::min<uint64_t>(b, data.size());
    if (b <= a) return {};
    return Bytes(data.begin() + static_cast<std::ptrdiff_t>(a), data.begin() + static_cast<std::ptrdiff_t>(b));
}

uint64_t fnv(const Bytes& data) { return vfs::fnv1a64(data.data(), data.size()); }

bool ends_with(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

/// pathlib's PurePath(p).name / .stem.
std::string py_name(const std::string& p) {
    const size_t slash = p.rfind('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}
std::string py_stem(const std::string& p) {
    const std::string name = py_name(p);
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == name.size()) return name;
    return name.substr(0, dot);
}

Bytes read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    Bytes out(static_cast<size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(out.data()), size))
        throw std::runtime_error("cannot read " + path.string());
    return out;
}

// ---------------------------------------------------------------------------------------------
// Disc containers (tools/disc/drv_unpack.py read_toc, tools/assets/dcb_containers.py read_pak).
// ---------------------------------------------------------------------------------------------

struct DrvFile {
    std::string path;  ///< "DIR/NAME.EXT"
    uint64_t offset = 0, size = 0;
};

std::string c_string(const uint8_t* p, size_t max) {
    size_t n = 0;
    while (n < max && p[n]) ++n;
    return std::string(reinterpret_cast<const char*>(p), n);
}

void walk_toc(const Bytes& drv, uint64_t sector, const std::string& prefix, int depth, std::vector<DrvFile>& files) {
    if (depth > 8) throw std::runtime_error("TOC nesting too deep; not a DRV?");
    for (uint64_t off = sector * kSector; off + 32 <= drv.size(); off += 32) {
        const uint8_t* e = drv.data() + off;
        const uint8_t kind = e[0];
        if (kind == 0) break;
        const std::string ext = c_string(e + 1, 3);
        const uint64_t start = load32(e + 4), size = load32(e + 8);
        const std::string name = c_string(e + 16, 16);
        if (kind == 0x80) {
            walk_toc(drv, start, prefix + name + "/", depth + 1, files);
        } else if (kind == 0x01) {
            // A stale record (US B.DRV's "copy of CARD2.CDD") points past the end: the game never
            // opens it.
            if (start * kSector + size <= drv.size()) files.push_back({prefix + name + "." + ext, start * kSector, size});
        } else {
            throw std::runtime_error("unknown DRV TOC entry type");
        }
    }
}

std::vector<DrvFile> read_toc(const Bytes& drv) {
    std::vector<DrvFile> files;
    walk_toc(drv, 0, "", 0, files);
    return files;
}

/// Where the first kind-5 chunk holding a TIS starts in a PAK (its data), or nothing (also for a
/// PAK whose chunk chain is broken).
std::optional<size_t> pak_tis_start(const uint8_t* data, size_t n) {
    std::vector<std::pair<uint16_t, size_t>> chunks;  // kind, data offset
    size_t off = 0;
    bool ended = false;
    while (off + 4 <= n) {
        if (load32(data + off) == 0xFFFFFFFFu) {
            ended = true;
            break;
        }
        if (off + 8 > n) return std::nullopt;
        const uint16_t kind = load16(data + off);
        const uint64_t size = load32(data + off + 4);
        if (off + 8 + size > n) return std::nullopt;  // overruns the file
        chunks.emplace_back(kind, off + 8);
        off += 8 + static_cast<size_t>(size);
    }
    if (!ended && off != n && std::any_of(data + off, data + n, [](uint8_t b) { return b != 0; }))
        return std::nullopt;  // trailing bytes
    for (const auto& [kind, at] : chunks) {
        if (kind == 5 && at + 2 <= n && data[at] == 'T' && data[at + 1] == 'p') return at;
    }
    return std::nullopt;
}

/// Byte offsets (in the entry) of the TIMs a DRV entry lists: an ARC offset table, or a TIS (a
/// .TIS file, or the kind-5 chunk of a PAK). Nothing for other entries.
std::optional<std::vector<uint64_t>> container_offsets(const uint8_t* data, size_t n, const std::string& path) {
    std::vector<uint64_t> out;
    if (ends_with(path, ".ARC")) {
        // u32 offsets, count = first offset / 4; the MATCH / WIN archives end the table with the
        // end of the file, which is left out.
        if (n < 4) return std::nullopt;
        const uint32_t first = load32(data);
        if (first % 4 || first < 4 || first > n) return std::nullopt;
        for (uint32_t i = 0; i < first / 4; ++i) {
            const uint32_t o = load32(data + 4 * i);
            if (o < n) out.push_back(o);
        }
        return out;
    }
    std::optional<size_t> base;
    if (ends_with(path, ".TIS")) {
        if (n >= 2 && data[0] == 'T' && data[1] == 'p') base = 0;
    } else if (ends_with(path, ".PAK")) {
        base = pak_tis_start(data, n);
    }
    if (!base) return std::nullopt;
    // TIS: "Tp", u16 n, u32 word_offset[n], then the TIMs.
    if (*base + 4 > n) return std::nullopt;
    const uint16_t count = load16(data + *base + 2);
    if (*base + 4 + 4 * uint64_t{count} > n) return std::nullopt;
    for (uint16_t i = 0; i < count; ++i) out.push_back(*base + 4 * uint64_t{load32(data + *base + 4 + 4 * i)});
    return out;
}

// ---------------------------------------------------------------------------------------------
// Pairing.
// ---------------------------------------------------------------------------------------------

/// A manifest entry, as far as the swap reads it.
struct Meta {
    uint64_t img = 0;
    int w = 0, h = 0, bpp = 0;
    std::string path;
};

/// variant_index: the N of "..._palN.png", else -1.
int variant_index(const Meta& e) {
    const std::string& p = e.path;
    if (!ends_with(p, ".png")) return -1;
    const size_t end = p.size() - 4;
    size_t start = end;
    while (start > 0 && p[start - 1] >= '0' && p[start - 1] <= '9') --start;
    if (start == end || start < 4 || p.compare(start - 4, 4, "_pal") != 0) return -1;
    return std::stoi(p.substr(start, end - start));
}

void sort_variants(std::vector<Meta>& v) {
    std::stable_sort(v.begin(), v.end(), [](const Meta& a, const Meta& b) { return variant_index(a) < variant_index(b); });
}

struct Item {
    Tim tim;
    std::vector<Meta> var;  ///< the manifest entries (palette variants) of this TIM
};
using Entries = std::map<std::string, std::vector<Item>>;  ///< DRV entry path -> its TIMs

struct Shape {
    int bpp;
    Rect image;
    bool has_clut;
    Rect clut;
    std::vector<std::pair<int, int>> var;
    auto operator<=>(const Shape&) const = default;
};

Shape shape_of(const Item& it) {
    Shape s{it.tim.bpp, it.tim.image, it.tim.has_clut, it.tim.has_clut ? it.tim.clut : Rect{}, {}};
    for (const Meta& e : it.var) s.var.emplace_back(e.w, e.h);
    return s;
}

struct Paired {
    std::vector<std::pair<size_t, size_t>> pairs;  ///< (index in a, index in b)
    size_t shared = 0;
};

/// Pair the TIMs of one entry by shape (swap_us_images.pair): TIMs that share a shape pair by
/// identical pixels first, then in file order. Nothing when the two sets of shapes differ.
std::optional<Paired> pair_items(const std::vector<Item>& a, const std::vector<Item>& b, bool partial) {
    using Groups = std::vector<std::pair<Shape, std::vector<size_t>>>;  // in first-seen order
    const auto group = [](const std::vector<Item>& items) {
        Groups g;
        std::map<Shape, size_t> at;
        for (size_t i = 0; i < items.size(); ++i) {
            Shape s = shape_of(items[i]);
            const auto [it, fresh] = at.emplace(s, g.size());
            if (fresh) g.emplace_back(std::move(s), std::vector<size_t>{});
            g[it->second].second.push_back(i);
        }
        return g;
    };
    Groups ga = group(a);
    const Groups gb = group(b);
    const auto find_b = [&gb](const Shape& s) -> const std::vector<size_t>* {
        for (const auto& [k, v] : gb)
            if (k == s) return &v;
        return nullptr;
    };
    if (partial) {  // keep only the shapes both sides have the same number of
        std::erase_if(ga, [&](const auto& g) {
            const std::vector<size_t>* vb = find_b(g.first);
            return g.second.size() != (vb ? vb->size() : 0);
        });
    } else {
        if (ga.size() != gb.size()) return std::nullopt;
        for (const auto& [k, va] : ga) {
            const std::vector<size_t>* vb = find_b(k);
            if (!vb || vb->size() != va.size()) return std::nullopt;
        }
    }
    Paired out;
    for (const auto& [k, va] : ga) {
        std::vector<size_t> vb = *find_b(k);
        if (va.size() == 1) {
            out.pairs.emplace_back(va[0], vb[0]);
            continue;
        }
        // The US build also swaps equal-shape TIMs inside a PAK (E 541): unchanged ones first.
        std::vector<size_t> rest_a;
        for (const size_t ta : va) {
            const auto same = std::find_if(vb.begin(), vb.end(), [&](size_t tb) { return b[tb].tim.pixels == a[ta].tim.pixels; });
            if (same != vb.end()) {
                out.pairs.emplace_back(ta, *same);
                vb.erase(same);
            } else {
                rest_a.push_back(ta);
            }
        }
        out.shared += rest_a.size() > 1 ? rest_a.size() : 0;
        for (size_t i = 0; i < rest_a.size() && i < vb.size(); ++i) out.pairs.emplace_back(rest_a[i], vb[i]);
    }
    return out;
}

/// US TIMs listed in NARROW as the JP-shaped image they stand for (pixels cut to the JP width).
std::vector<Item> narrowed(const std::string& drv, std::vector<Item> items) {
    for (Item& it : items) {
        for (const NarrowRule& n : kNarrow) {
            if (drv != n.drv || it.tim.image != n.us) continue;
            Tim& t = it.tim;
            t.pixels = narrow_columns(t.pixels, t.bpp, t.image.w * 16 / t.bpp, n.jp.w * 16 / t.bpp, n.right);
            t.image = n.jp;
        }
    }
    return items;
}

/// US TIMs whose palette is a JP palette uploaded in another shape, as the JP-shaped TIM.
std::vector<Item> reshaped(const std::vector<Item>& jp, std::vector<Item> us) {
    std::map<std::pair<int, Rect>, std::vector<size_t>> by_image;
    for (size_t i = 0; i < jp.size(); ++i) by_image[{jp[i].tim.bpp, jp[i].tim.image}].push_back(i);
    for (Item& it : us) {
        const auto found = by_image.find({it.tim.bpp, it.tim.image});
        if (found == by_image.end() || found->second.size() != 1) continue;
        const Item& j = jp[found->second[0]];
        if (!is_palette_reshape(j.tim, it.tim)) continue;
        it.tim.clut = j.tim.clut;
        if (!it.var.empty()) {  // TIS / ARC images read from the disc carry no variants on either side
            std::vector<Meta> v;
            for (const Meta& e : j.var) {
                Meta m = it.var[0];
                m.w = e.w;
                m.h = e.h;
                v.push_back(std::move(m));
            }
            it.var = std::move(v);
        }
    }
    return us;
}

/// The US TIM paired with `jt`, recomposed when COMPOSE lists the image.
Tim composed(const std::string& drv, const Tim& jt, Tim ut) {
    for (const ComposeRule& c : kCompose) {
        if (drv != c.drv || jt.image != c.image) continue;
        auto [pixels, palette] = compose_image(ut, jt, c.blocks);
        ut.pixels = std::move(pixels);
        ut.palette = std::move(palette);
        break;
    }
    return ut;
}

bool is_composed(const std::string& drv, const Rect& image) {
    return std::any_of(kCompose.begin(), kCompose.end(), [&](const ComposeRule& c) { return drv == c.drv && image == c.image; });
}

/// The US pixel data as the JP game should upload it (JP_DRAW_SCALE images pre-warped).
Bytes pixels_for_jp(const std::string& drv, const Tim& us) {
    for (const DrawScale& s : kJpDrawScale) {
        if (drv == s.drv && us.image == s.image)
            return prewarp_columns(us.pixels, us.bpp, us.image.w * 16 / us.bpp, s.read, s.shown);
    }
    return us.pixels;
}

const char* keep_jp(const std::string& drv, const Rect& image) {
    for (const KeepRect& k : kKeepJp)
        if (drv == k.drv && image == k.image) return k.why;
    for (const KeepAt& k : kKeepJpAt)
        if (drv == k.drv && image.x == k.x && image.y == k.y) return k.why;
    return nullptr;
}

/// A manifest entry for a TIS TIM the ripper did not list, named the way the ripper names its
/// images (C_OBJECT_UNIT_off0001268c_84x123.png).
Meta tis_entry(const Tim& t, uint64_t key, const std::string& drv, const std::string& path, uint64_t entry_offset) {
    const int w = t.bpp <= 16 ? t.image.w * 16 / t.bpp : t.image.w * 2 / 3;
    std::string stem = drv.substr(0, 1) + "_" + path.substr(0, path.rfind('.'));
    for (char& c : stem) {
        const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        c = word ? static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c) : '_';
    }
    char tail[64];
    std::snprintf(tail, sizeof tail, "_off%08llx_%dx%d.png", static_cast<unsigned long long>(t.offset - entry_offset), w,
                  static_cast<int>(t.image.h));
    return {key, w, t.image.h, t.bpp, stem + tail};
}

struct Candidate {
    Bytes us;           ///< the US upload payload
    Bytes jp;           ///< the JP payload it replaces
    std::string label;  ///< "DRV:entry" it came from
    uint64_t image = 0; ///< palettes: the JP image hash of the TIM the palette belongs to
};

/// Pick one US payload for a JP upload that several pairs map to (swap_us_images.resolve): US
/// copies identical to the JP data are dropped, then the entry the JP file is named after wins.
const Candidate* resolve(const std::string& key_name, const std::vector<Candidate>& cands) {
    std::vector<const Candidate*> distinct;
    for (const Candidate& c : cands) {
        if (std::none_of(distinct.begin(), distinct.end(), [&](const Candidate* d) { return d->us == c.us; }))
            distinct.push_back(&c);
    }
    std::vector<const Candidate*> changed;
    for (const Candidate* c : distinct)
        if (c->us != c->jp) changed.push_back(c);
    if (changed.empty()) return nullptr;
    if (distinct.size() == 1) return changed[0];
    // re.match(r"[A-Z]_(.+?)_off", key_name)
    std::optional<std::string> own;
    if (key_name.size() >= 2 && key_name[0] >= 'A' && key_name[0] <= 'Z' && key_name[1] == '_') {
        const size_t off = key_name.find("_off", 3);
        if (off != std::string::npos) own = key_name.substr(2, off - 2);
    }
    for (const Candidate* c : changed) {
        if (own && py_stem(c->label.substr(c->label.find(':') + 1)) == *own) return c;
    }
    return changed[0];
}

/// One game's textures: the DRVs the pairing reads and the ripper's manifest, grouped by TIM.
struct Side {
    std::map<std::string, Bytes> drvs;                                    ///< kept DRV bytes
    std::map<std::string, std::map<uint32_t, std::vector<Meta>>> by_tim;  ///< drv -> offset -> variants
};

/// For each DRV entry: its ripped TIMs in file order, with their manifest variants.
Entries entry_tims(const Bytes& drv, const std::vector<DrvFile>& toc, const std::map<uint32_t, std::vector<Meta>>& by_tim) {
    struct Span {
        uint64_t start, end;
        const std::string* path;
    };
    std::vector<Span> spans;
    for (const DrvFile& f : toc) spans.push_back({f.offset, f.offset + f.size, &f.path});
    std::sort(spans.begin(), spans.end(), [](const Span& x, const Span& y) {
        if (x.start != y.start) return x.start < y.start;
        if (x.end != y.end) return x.end < y.end;
        return *x.path < *y.path;
    });
    Entries out;
    for (const auto& [off, variants] : by_tim) {
        // bisect_right(starts, off) - 1
        const auto after = std::upper_bound(spans.begin(), spans.end(), uint64_t{off},
                                            [](uint64_t v, const Span& s) { return v < s.start; });
        if (after == spans.begin()) continue;
        const Span& s = *std::prev(after);
        if (!(s.start <= off && off < s.end)) continue;
        if (std::optional<Tim> tim = read_tim(drv, off)) out[*s.path].push_back({std::move(*tim), variants});
    }
    for (auto& [path, items] : out)
        std::stable_sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.tim.offset < y.tim.offset; });
    return out;
}

/// For each DRV entry matching `rx` that lists its TIMs (a TIS or an ARC): its TIMs in file order.
Entries tis_entry_tims(const Bytes& drv, const std::vector<DrvFile>& toc, const std::regex& rx) {
    Entries out;
    for (const DrvFile& f : toc) {
        if (!std::regex_search(f.path, rx)) continue;
        const auto offsets = container_offsets(drv.data() + f.offset, static_cast<size_t>(f.size), f.path);
        if (!offsets) continue;
        std::vector<Item> items;
        for (const uint64_t o : *offsets)
            if (std::optional<Tim> t = read_tim(drv, static_cast<size_t>(f.offset + o))) items.push_back({std::move(*t), {}});
        out[f.path] = std::move(items);
    }
    return out;
}

std::vector<fs::path> drv_files(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string ext = e.path().extension().string();
        for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (e.is_regular_file() && ext == ".drv") out.push_back(e.path());
    }
    if (ec) throw std::runtime_error("cannot list " + dir.string());
    std::sort(out.begin(), out.end());
    return out;
}

/// Rip one DRV into `side` (variants by TIM); keeps the bytes when `keep`.
void rip_into(vfs::Ripper& ripper, Side& side, const fs::path& path, bool keep, Bytes& bytes) {
    const std::string name = path.filename().string();
    bytes = read_file(path);
    const size_t first = ripper.manifest().size();
    ripper.rip_drv(name, bytes);
    auto& by_off = side.by_tim[name];
    for (size_t i = first; i < ripper.manifest().size(); ++i) {
        const vfs::RipEntry& e = ripper.manifest()[i];
        by_off[e.drv_offset].push_back({e.img, e.w, e.h, e.bpp, e.path});
    }
    for (auto& [off, v] : by_off) sort_variants(v);
    if (by_off.empty()) side.by_tim.erase(name);
    if (keep) side.drvs[name] = bytes;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Helpers (exposed for tests).
// ---------------------------------------------------------------------------------------------

std::optional<Tim> read_tim(const std::vector<uint8_t>& drv, size_t off) {
    if (uint64_t{off} + 8 > drv.size() || load32(drv.data() + off) != 0x10) return std::nullopt;
    const uint32_t flags = load32(drv.data() + off + 4);
    uint64_t p = uint64_t{off} + 8;
    Tim t;
    t.offset = static_cast<uint32_t>(off);
    const auto block = [&](Rect& r, Bytes& payload) {
        if (p + 12 > drv.size()) return false;
        const uint8_t* b = drv.data() + p;
        const uint32_t len = load32(b);
        r = {load16(b + 4), load16(b + 6), load16(b + 8), load16(b + 10)};
        payload = slice(drv, p + 12, p + len);
        p += len;
        return true;
    };
    if (flags & 8) {
        t.has_clut = true;
        if (!block(t.clut, t.palette)) return std::nullopt;
    }
    if (!block(t.image, t.pixels)) return std::nullopt;
    static constexpr int kBpp[] = {4, 8, 16, 24};
    t.bpp = kBpp[flags & 3];
    return t;
}

namespace {
std::vector<int> unpack4(const uint8_t* row, int width) {
    std::vector<int> idx(static_cast<size_t>(width));
    for (int x = 0; x < width; ++x) idx[static_cast<size_t>(x)] = (x & 1) ? row[x / 2] >> 4 : row[x / 2] & 15;
    return idx;
}
void pack4(const std::vector<int>& idx, Bytes& out) {
    for (size_t x = 0; x + 1 < idx.size(); x += 2) out.push_back(static_cast<uint8_t>(idx[x] | (idx[x + 1] << 4)));
}
}  // namespace

std::vector<uint8_t> prewarp_columns(const std::vector<uint8_t>& pixels, int bpp, int width, int read, int shown) {
    if (bpp != 4 || width <= 0 || width % 2 || shown <= 0 || pixels.size() % static_cast<size_t>(width / 2))
        throw std::invalid_argument("prewarp_columns takes 4-bit rows of an even width");
    // Screen column i samples texel floor(i * read / shown): source column i goes there; texels no
    // column samples repeat their left neighbour.
    std::vector<int> src_of(static_cast<size_t>(width), -1);
    for (int i = 0; i < width; ++i) {
        const int t = i * read / shown;
        if (t >= width) break;
        src_of[static_cast<size_t>(t)] = i;
    }
    for (size_t t = 1; t < src_of.size(); ++t)
        if (src_of[t] < 0) src_of[t] = src_of[t - 1];
    const size_t stride = static_cast<size_t>(width / 2);
    Bytes out;
    out.reserve(pixels.size());
    for (size_t r = 0; r < pixels.size(); r += stride) {
        const std::vector<int> idx = unpack4(pixels.data() + r, width);
        std::vector<int> fresh(idx.size());
        for (size_t x = 0; x < idx.size(); ++x) fresh[x] = idx[static_cast<size_t>(src_of[x])];
        pack4(fresh, out);
    }
    return out;
}

std::vector<uint8_t> narrow_columns(const std::vector<uint8_t>& pixels, int bpp, int width, int new_width, int right) {
    if (bpp != 4 || width <= 0 || width % 2 || new_width % 2 || !(0 <= right && right <= new_width && new_width <= width) ||
        pixels.size() % static_cast<size_t>(width / 2))
        throw std::invalid_argument("narrow_columns takes 4-bit rows of an even width, cut to a smaller one");
    const size_t stride = static_cast<size_t>(width / 2);
    Bytes out;
    for (size_t r = 0; r < pixels.size(); r += stride) {
        const std::vector<int> idx = unpack4(pixels.data() + r, width);
        std::vector<int> cut(idx.begin(), idx.begin() + (new_width - right));
        cut.insert(cut.end(), idx.end() - right, idx.end());
        pack4(cut, out);
    }
    return out;
}

bool is_palette_reshape(const Tim& jp, const Tim& us) {
    if (!jp.has_clut || !us.has_clut || jp.bpp != us.bpp || jp.image != us.image || jp.clut.x != us.clut.x ||
        jp.clut.y != us.clut.y || (jp.clut.w == us.clut.w && jp.clut.h == us.clut.h) ||
        jp.clut.w * jp.clut.h != us.clut.w * us.clut.h || jp.palette.size() != us.palette.size() ||
        jp.palette.size() != 2u * jp.clut.w * jp.clut.h)
        return false;
    for (size_t i = 0; i < jp.palette.size(); i += 2) {
        if ((load16(jp.palette.data() + i) ^ load16(us.palette.data() + i)) & 0x7FFF) return false;
    }
    return true;
}

std::pair<std::vector<uint8_t>, std::vector<uint8_t>> compose_image(const Tim& us, const Tim& jp,
                                                                     const std::vector<Block>& blocks) {
    if (us.bpp != 4 || jp.bpp != 4 || us.image.w != jp.image.w || us.image.h != jp.image.h || us.palette.size() != 32 ||
        jp.palette.size() != 32)
        throw std::invalid_argument("compose_image takes two 4-bit images of one size with 16-colour palettes");
    const int width = us.image.w * 4, height = us.image.h;
    const auto grid = [&](const Bytes& px) {
        std::vector<std::vector<int>> g(static_cast<size_t>(height));
        for (int y = 0; y < height; ++y) g[static_cast<size_t>(y)] = unpack4(px.data() + y * width / 2, width);
        return g;
    };
    if (us.pixels.size() < static_cast<size_t>(width / 2 * height) || jp.pixels.size() < static_cast<size_t>(width / 2 * height))
        throw std::invalid_argument("compose_image: short pixel data");
    const auto src_us = grid(us.pixels), src_jp = grid(jp.pixels);
    auto out = src_us;
    std::vector<std::vector<bool>> from_jp(static_cast<size_t>(height), std::vector<bool>(static_cast<size_t>(width)));
    for (const Block& b : blocks) {
        const auto& src = b.from_jp ? src_jp : src_us;
        for (int y = 0; y < b.h; ++y)
            for (int x = 0; x < b.w; ++x) {
                const auto dy = static_cast<size_t>(b.dy + y), dx = static_cast<size_t>(b.dx + x);
                out[dy][dx] = src[static_cast<size_t>(b.sy + y)][static_cast<size_t>(b.sx + x)];
                from_jp[dy][dx] = b.from_jp;
            }
    }
    uint16_t us_pal[16], jp_pal[16];
    for (int k = 0; k < 16; ++k) {
        us_pal[k] = load16(us.palette.data() + 2 * k);
        jp_pal[k] = load16(jp.palette.data() + 2 * k);
    }
    const auto distance = [](uint16_t a, uint16_t b) {
        int d = 0;
        for (int s : {0, 5, 10}) {
            const int c = ((a >> s) & 31) - ((b >> s) & 31);
            d += c * c;
        }
        return d;
    };
    std::set<int> used, needed;
    for (size_t y = 0; y < out.size(); ++y)
        for (size_t x = 0; x < out[y].size(); ++x) (from_jp[y][x] ? needed : used).insert(out[y][x]);
    std::map<int, int> index;
    std::vector<int> missing;
    for (const int j : needed) {
        std::vector<int> exact;
        for (int k = 0; k < 16; ++k)
            if (us_pal[k] == jp_pal[j]) exact.push_back(k);
        if (exact.empty()) {
            missing.push_back(j);
            continue;
        }
        const auto in_use = std::find_if(exact.begin(), exact.end(), [&](int k) { return used.count(k) != 0; });
        index[j] = in_use != exact.end() ? *in_use : exact[0];
        used.insert(index[j]);
    }
    std::vector<int> free_slots;
    for (int k = 1; k < 16; ++k)
        if (!used.count(k)) free_slots.push_back(k);
    const auto nearest_us = [&](int j) {
        int best = distance(jp_pal[j], us_pal[0]);
        for (int k = 1; k < 16; ++k) best = std::min(best, distance(jp_pal[j], us_pal[k]));
        return best;
    };
    // The JP colours farthest from any US colour take the free slots first.
    std::stable_sort(missing.begin(), missing.end(), [&](int a, int b) { return nearest_us(a) > nearest_us(b); });
    for (const int j : missing) {
        if (!free_slots.empty()) {
            index[j] = free_slots.front();
            free_slots.erase(free_slots.begin());
            us_pal[index[j]] = jp_pal[j];
        } else {
            int best = 0;
            for (int k = 1; k < 16; ++k)
                if (distance(jp_pal[j], us_pal[k]) < distance(jp_pal[j], us_pal[best])) best = k;
            index[j] = best;
        }
    }
    for (size_t y = 0; y < out.size(); ++y)
        for (size_t x = 0; x < out[y].size(); ++x)
            if (from_jp[y][x]) out[y][x] = index[out[y][x]];
    Bytes pixels, palette;
    for (const auto& row : out) pack4(row, pixels);
    for (const uint16_t v : us_pal) {
        palette.push_back(static_cast<uint8_t>(v & 0xFF));
        palette.push_back(static_cast<uint8_t>(v >> 8));
    }
    return {std::move(pixels), std::move(palette)};
}

std::optional<std::vector<uint8_t>> fit_image(const Tim& jp, const std::vector<uint8_t>& rgba, int w, int h,
                                              int slot_w) {
    if ((jp.bpp != 4 && jp.bpp != 8) || !jp.has_clut || w <= 0 || h <= 0) return std::nullopt;
    const int jp_w = jp.image.w * 16 / jp.bpp, jp_h = jp.image.h;
    const int tw = slot_w ? slot_w : jp_w, th = jp_h;
    const int per_unit = 16 / jp.bpp;
    if (tw < jp_w || tw % per_unit) return std::nullopt;
    std::vector<uint8_t> fit;
    if (!vfs::downsample_rgba(rgba, w, h, tw, th, fit)) return std::nullopt;
    // The JP image's own palette: row 0, the entries an index can reach (the manifest "pal").
    const size_t reach = std::min<size_t>(jp.clut.w, jp.bpp == 4 ? 16 : 256);
    if (jp.palette.size() < 2 * reach) return std::nullopt;
    std::vector<uint16_t> pal(reach);
    for (size_t k = 0; k < reach; ++k) pal[k] = load16(jp.palette.data() + 2 * k);
    const bool same_size = tw == jp_w;
    if (same_size && jp.pixels.size() < static_cast<size_t>(jp_w) * static_cast<size_t>(th) / static_cast<size_t>(per_unit) * 2)
        return std::nullopt;
    std::vector<uint8_t> out(static_cast<size_t>(tw) * static_cast<size_t>(th) * static_cast<size_t>(jp.bpp) / 8);
    for (size_t i = 0; i < static_cast<size_t>(tw) * static_cast<size_t>(th); ++i) {
        const uint8_t* px = &fit[i * 4];
        unsigned index = 0;
        bool kept = false;
        if (same_size) {
            const unsigned orig = jp.bpp == 8 ? jp.pixels[i] : (jp.pixels[i / 2] >> ((i & 1) * 4)) & 15u;
            if (orig < reach && pal[orig] == vfs::rgba_to_psx15(px[0], px[1], px[2], px[3])) {
                index = orig;
                kept = true;
            }
        }
        if (!kept) index = vfs::palette_index(pal.data(), reach, px[0], px[1], px[2], px[3]);
        if (jp.bpp == 8) out[i] = static_cast<uint8_t>(index);
        else out[i / 2] = static_cast<uint8_t>(out[i / 2] | (index << ((i & 1) * 4)));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// plan(): swap_us_images.main() up to the manifest it writes.
// ---------------------------------------------------------------------------------------------

std::optional<Plan> plan(const fs::path& jp_fs, const fs::path& us_fs, const StepFn& step) {
    struct Group {
        const char* drv;
        std::regex rx;
        bool whole_tis;
    };
    std::vector<Group> groups;
    std::set<std::string> group_drvs;
    for (const EntryGroup& g : kDefaultEntries) groups.push_back({g.drv, std::regex(std::string(g.pattern) + "$"), false});
    for (const EntryGroup& g : kTisEntries) groups.push_back({g.drv, std::regex(std::string(g.pattern) + "$"), true});
    for (const Group& g : groups) group_drvs.insert(g.drv);
    const std::regex never(kNever), partial_rx(kPartial);

    const std::vector<fs::path> jp_paths = drv_files(jp_fs);
    std::vector<fs::path> us_paths;
    for (const fs::path& p : drv_files(us_fs))
        if (group_drvs.count(p.filename().string())) us_paths.push_back(p);
    const uint64_t total = jp_paths.size() + us_paths.size() + groups.size() + 1;
    uint64_t done = 0;
    const auto next = [&] { return !step || step(done++, total); };

    // 1. The textures of both games, indexed like dcb_asset_ripper does. Every JP DRV, in the
    //    ripper's order (its manifest names identical art after the first copy, and the palette
    //    check below looks at every JP image); on the US side only the DRVs the pairing reads.
    Side jp, us;
    std::set<uint64_t> existing;                       // every JP manifest hash
    std::map<uint64_t, std::vector<Meta>> jp_by_hash;  // for TIS TIMs: JP manifest entries by hash
    struct PaletteUse {
        std::string drv;
        uint32_t off;
        uint64_t palette, pixels;
    };
    std::vector<PaletteUse> palette_uses;  // every JP TIM with a palette
    {
        vfs::Ripper ripper;
        Bytes bytes;
        for (const fs::path& p : jp_paths) {
            if (!next()) return std::nullopt;
            const std::string name = p.filename().string();
            rip_into(ripper, jp, p, group_drvs.count(name) != 0, bytes);
            const auto found = jp.by_tim.find(name);
            if (found == jp.by_tim.end()) continue;
            for (const auto& [off, v] : found->second) {
                const std::optional<Tim> t = read_tim(bytes, off);
                if (t && !t->palette.empty()) palette_uses.push_back({name, off, fnv(t->palette), fnv(t->pixels)});
            }
        }
        for (const vfs::RipEntry& e : ripper.manifest()) {
            existing.insert(e.img);
            if (!e.drv.empty()) jp_by_hash[e.img].push_back({e.img, e.w, e.h, e.bpp, e.path});
        }
    }
    {
        vfs::Ripper ripper;
        Bytes bytes;
        for (const fs::path& p : us_paths) {
            if (!next()) return std::nullopt;
            rip_into(ripper, us, p, true, bytes);
        }
    }

    // 2. Pair the images of each entry.
    Plan result;
    std::map<uint64_t, std::vector<Candidate>> images;  // JP image hash -> candidates
    std::map<uint64_t, Meta> image_meta;                // JP image hash -> a JP manifest entry
    std::map<uint64_t, std::vector<Candidate>> palettes;
    std::map<uint64_t, std::pair<int, int>> palette_rect;
    std::set<std::pair<std::string, uint32_t>> jp_only;  // JP TIMs in entries the US build does not have
    std::map<std::string, std::vector<DrvFile>> tocs[2];
    const auto toc_of = [&](int s, const std::string& drv, const Bytes& bytes) -> const std::vector<DrvFile>& {
        auto found = tocs[s].find(drv);
        if (found == tocs[s].end()) found = tocs[s].emplace(drv, read_toc(bytes)).first;
        return found->second;
    };
    for (const Group& g : groups) {
        if (!next()) return std::nullopt;
        const std::string drv = g.drv;
        if (!jp.by_tim.count(drv) || !us.by_tim.count(drv) || !jp.drvs.count(drv) || !us.drvs.count(drv)) continue;
        const Bytes& jp_drv = jp.drvs.at(drv);
        const Bytes& us_drv = us.drvs.at(drv);
        const std::vector<DrvFile>& jp_toc = toc_of(0, drv, jp_drv);
        const std::vector<DrvFile>& us_toc = toc_of(1, drv, us_drv);
        std::map<std::string, uint64_t> jp_starts;
        for (const DrvFile& f : jp_toc) jp_starts[f.path] = f.offset;
        Entries jp_ents, us_ents;
        if (g.whole_tis) {
            jp_ents = tis_entry_tims(jp_drv, jp_toc, g.rx);
            us_ents = tis_entry_tims(us_drv, us_toc, g.rx);
        } else {
            jp_ents = entry_tims(jp_drv, jp_toc, jp.by_tim.at(drv));
            us_ents = entry_tims(us_drv, us_toc, us.by_tim.at(drv));
            for (const auto& [path, items] : jp_ents)
                if (!us_ents.count(path))
                    for (const Item& it : items) jp_only.emplace(drv, it.tim.offset);
        }
        for (const auto& [path, jp_items] : jp_ents) {
            const std::string label = drv + ":" + path;
            if (!std::regex_search(path, g.rx) || std::regex_search(path, never)) continue;
            const auto us_found = us_ents.find(path);
            if (us_found == us_ents.end()) {  // no US entry
                ++result.skipped_entries;
                continue;
            }
            const std::vector<Item> us_items = reshaped(jp_items, narrowed(drv, us_found->second));
            const std::optional<Paired> paired = pair_items(jp_items, us_items, std::regex_search(label, partial_rx));
            if (!paired) {  // layout differs
                ++result.skipped_entries;
                continue;
            }
            for (const auto& [ja, ub] : paired->pairs) {
                const Tim& jt = jp_items[ja].tim;
                Tim ut = us_items[ub].tim;
                const uint64_t key = fnv(jt.pixels);
                if (keep_jp(drv, jt.image) && ut.pixels != jt.pixels) continue;
                if (is_composed(drv, jt.image) && ut.pixels != jt.pixels) ut = composed(drv, jt, std::move(ut));
                std::vector<Meta> jv = jp_items[ja].var;
                if (g.whole_tis) {  // the replacer keys by hash: any entry of these pixels will do
                    const auto by_hash = jp_by_hash.find(key);
                    if (by_hash != jp_by_hash.end()) {
                        jv = by_hash->second;
                        sort_variants(jv);
                    } else {
                        jv = {tis_entry(jt, key, drv, path, jp_starts[path])};
                    }
                }
                if (jv.empty() || key != jv[0].img) continue;  // the ripper hashed another upload shape
                images[key].push_back({pixels_for_jp(drv, ut), jt.pixels, label, 0});
                image_meta[key] = jv[0];
                if (!jt.palette.empty()) {
                    const uint64_t pkey = fnv(jt.palette);
                    if (ut.palette != jt.palette) {
                        palettes[pkey].push_back({ut.palette, jt.palette, label, key});
                        palette_rect[pkey] = {jt.clut.w, jt.clut.h};
                    }
                }
            }
        }
    }

    // 3. A JP palette also used by TIMs outside the swap would recolour them: it is kept JP when
    //    a regular-game image outside the swap uses it (JP-only images may take the US colours).
    std::set<uint64_t> outside;
    for (const PaletteUse& u : palette_uses) {
        if (!palettes.count(u.palette) || images.count(u.pixels)) continue;
        const bool jp_only_drv = std::any_of(std::begin(kJpOnlyDrvs), std::end(kJpOnlyDrvs),
                                             [&](const char* d) { return u.drv == d; });
        if (!jp_only_drv && !jp_only.count({u.drv, u.off})) outside.insert(u.palette);
    }

    // 4. One US payload per JP upload.
    std::map<uint64_t, const Candidate*> image_plan;
    for (const auto& [key, cands] : images) {
        if (const Candidate* pick = resolve(py_name(image_meta.at(key).path), cands)) image_plan[key] = pick;
        else ++result.identical;
    }
    for (const auto& [key, pick] : image_plan) {
        const Meta& m = image_meta.at(key);
        result.images.push_back({key, m.w, m.h, m.bpp, pick->label, py_stem(m.path), pick->us, 0});
    }
    for (const auto& [pkey, cands] : palettes) {
        if (outside.count(pkey)) continue;
        // The palette of the pair whose image was picked, so a picked image never shows with
        // another pair's palette.
        const Candidate* pick = nullptr;
        for (const Candidate& c : cands) {
            const auto planned = image_plan.find(c.image);
            if (planned != image_plan.end() && planned->second->label == c.label) {
                pick = &c;
                break;
            }
        }
        if (!pick) pick = resolve("", cands);
        if (!pick || existing.count(pkey)) continue;  // a palette that is also a ripped image: leave it
        const auto [w, h] = palette_rect.at(pkey);
        result.palettes.push_back(
            {pkey, w, h, 16, pick->label + " (palette)", py_stem(image_meta.at(pick->image).path), pick->us, 0});
    }

    // 5. FIT: US art fitted into JP images whose entry does not pair (the title).
    for (const FitRule& r : kFit) {
        if (!jp.drvs.count(r.drv) || !us.drvs.count(r.drv)) continue;
        const Bytes* drv_bytes[2] = {&jp.drvs.at(r.drv), &us.drvs.at(r.drv)};
        std::optional<Tim> tims[2];
        for (int side = 0; side < 2; ++side) {
            const Bytes& bytes = *drv_bytes[side];
            for (const DrvFile& f : toc_of(side, r.drv, bytes)) {
                if (f.path != r.entry) continue;
                const auto offsets = container_offsets(bytes.data() + f.offset, static_cast<size_t>(f.size), f.path);
                for (const uint64_t o : offsets ? *offsets : std::vector<uint64_t>{}) {
                    std::optional<Tim> t = read_tim(bytes, static_cast<size_t>(f.offset + o));
                    if (t && t->image == (side == 0 ? r.jp : r.us)) tims[side] = std::move(t);
                }
            }
        }
        if (!tims[0] || !tims[1]) continue;
        const uint64_t key = fnv(tims[0]->pixels);
        const auto by_hash = jp_by_hash.find(key);
        if (by_hash == jp_by_hash.end() || images.count(key)) continue;
        std::vector<Meta> jv = by_hash->second;
        sort_variants(jv);
        // The US art as the ripper renders it (palette row 0): what a PNG of it would hold.
        const Bytes& us_drv = *drv_bytes[1];
        vfs::Tim ut;
        std::vector<uint8_t> rgba;
        if (!vfs::parse_tim(us_drv.data() + tims[1]->offset, us_drv.size() - tims[1]->offset, ut) ||
            !vfs::tim_to_rgba(ut, 0, rgba))
            continue;
        std::optional<Bytes> data = fit_image(*tims[0], rgba, ut.pixel_width(), ut.pixel_height(), r.slot_w);
        if (!data) continue;
        result.images.push_back({key, jv[0].w, jv[0].h, jv[0].bpp, std::string(r.drv) + ":" + r.entry, py_stem(jv[0].path),
                                 std::move(*data), r.slot_w});
    }
    std::sort(result.images.begin(), result.images.end(),
              [](const Replacement& a, const Replacement& b) { return a.key < b.key; });
    if (!next()) return std::nullopt;
    return result;
}

}  // namespace patch::art
