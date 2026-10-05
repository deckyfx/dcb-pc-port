// VS-screen big names in English (config/SLPS-03101/overrides.json, docs/re/text-engine.md §7.11).
//
// Before a card battle the VS screen shows the players' names in 32-px letters. The KAWSEG task
// vs_names_load (801F04FC, spawned by the VS screen 801F2B9C with a0 = mode, a1 = match number,
// a2 = the VS task) prepares them:
//
//   - for each name drawn from the font (mode != 0: the player only; mode 0, a battle with a
//     friend: both), it spawns bigname_load (80044684, a0 = name, a1 = row, a2 = parent) and
//     sleeps until that task wakes it. bigname_load sprintf's "B:\FONT\%04X.tim" for each 2-byte
//     Shift-JIS character (at most 8), loads it through file_load_task (8001B388), uploads it
//     with tim_upload (8001B67C) to (704 + i*8, 448 + row*32) with its CLUT at (752, 471 + row),
//     DrawSyncs, frees it, then wakes the parent. g_bigname_busy (80070E70) is 1 meanwhile.
//   - it loads B:\MATCH\NNN.ARC (NNN = the opponent's match number, 999 for mode 0) and uploads
//     every TIM in it at the TIM's own position; the last one is the opponent's name picture
//     (704, 480), whose width (tim prect w * 4) becomes the opponent's name width.
//   - name widths go to battle_player+0x114 (s16): (strlen(name)/2) * 32 for a font name.
//
// An ASCII name (the JP loader asks for files such as FONT\4A6F.tim that do not exist, uploads
// a NULL TIM, and the width formula halves odd lengths) is drawn here with the US big font
// (en_bigfont.bin from tools/text/bigfont.py: 16x32 glyphs, as the US FONT.ARC loader 80041CA8
// does): glyph i at (704 + i*4, 448 + row*32), at most 12 (the CLUTs sit at x 752, right after
// 12 glyphs), width 16 per character. Shift-JIS names take the JP original. The opponent's
// English name picture comes from the grafted MATCH archives (same tool); the archive path is
// the original's.
//
// vs_names_load is re-implemented whole (a wrapper cannot correct the widths in time: the VS
// screen runs concurrently and draws with them during the task's final 10-frame sleep); every
// step, stack argument and global write mirrors the MIPS routine.

#include "native_files.hpp"
#include "text.hpp"

#include <psx/recomp.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

extern "C" {
void dcb_task_sleep(PsxContext* ctx);  // tasks.cpp (80014D3C): sleep a0 frames, v0 = wake value
void f_800149E4(PsxContext* ctx);      // task_current_id() -> id
void f_80014AEC(PsxContext* ctx);      // task_spawn(0, -1, 0, stack, entry@sp16, args@sp20..)
void f_80014B7C(PsxContext* ctx);      // task_wake(id, value)
void f_8001B0D4(PsxContext* ctx);      // mem_free(p)
void f_8001B67C(PsxContext* ctx);      // tim_upload(tim, x, y, clut_x, clut_y@sp16); -1 keeps the TIM's
void f_8006740C(PsxContext* ctx);      // DrawSync(mode)
void f_8006BD60(PsxContext* ctx);      // strlen(s)
void f_8006C7B0(PsxContext* ctx);      // sprintf(buf, fmt, ...)
}

namespace {

constexpr int kV0 = 2, kA0 = 4, kA1 = 5, kA2 = 6, kA3 = 7, kSp = 29, kRa = 31;

constexpr uint32_t kLoadTask = 0x80044684u;      // bigname_load (task entry)
constexpr uint32_t kFileLoadTask = 0x8001B388u;  // file_load_task(path, parent) -> wakes with the buffer
constexpr uint32_t kBignameBusy = 0x80070E70u;   // g_bigname_busy
constexpr uint32_t kPlayerRecords = 0x80070C2Cu; // -> the two players' records, 10040 bytes each
constexpr uint32_t kRecordStride = 10040;
constexpr uint32_t kBattlePlayers = 0x801DAF40u; // battle_player* [2]
constexpr uint32_t kNameOff = 0x1CA;             // battle_player: name
constexpr uint32_t kWidthOff = 0x114;            // battle_player: big-name width (s16)
constexpr uint32_t kTimPrect = 0x801D743Cu;      // g_tim_image.prect (last TIM read)
constexpr uint32_t kFrameWait = 0x8007C0D0u;     // frames to sleep after an upload
constexpr uint32_t kMatchFormat = 0x801E0E1Cu;   // KAWSEG "B:\MATCH\%3.3d.ARC"
constexpr uint32_t kVsBusy = 0x801FF1E8u;        // KAWSEG: 1 while vs_names_load runs
constexpr uint32_t kVsAnim = 0x801FE788u;        // KAWSEG: four 32-byte slide-in records

constexpr int kVramX = 704, kVramY = 448, kClutX = 752, kClutY = 471;
constexpr size_t kMaxAscii = 12;  // 12 glyphs x 4 halfwords end where the CLUTs start (x 752)
constexpr int kGlyphW = 16;

uint32_t rd32(PsxContext& ctx, uint32_t a) { return psx_read32(&ctx, a); }
void wr32(PsxContext& ctx, uint32_t a, uint32_t v) { psx_write32(&ctx, a, v); }

// --- en_bigfont.bin (tools/text/bigfont.py) ---

constexpr int kFirst = 0x20, kCount = 92, kGlyphBytes = 256;

struct BigFont {
    bool tried = false;
    bool ok = false;
    uint8_t clut[32] = {};
    uint8_t present[kCount] = {};
    std::vector<uint8_t> pixels;  // kCount x 256 bytes
};

BigFont& font() {
    static BigFont f;
    if (f.tried) return f;
    f.tried = true;
    const std::string path = dcb::asset_path("en_bigfont.bin");
    FILE* in = std::fopen(path.c_str(), "rb");
    if (!in) return f;
    uint8_t hdr[8];
    f.pixels.resize(static_cast<size_t>(kCount) * kGlyphBytes);
    const bool ok = std::fread(hdr, 1, 8, in) == 8 && hdr[0] == 'B' && hdr[1] == 'G' && hdr[2] == 'F' &&
                    hdr[3] == '1' && hdr[4] == kFirst && hdr[5] == kCount && hdr[6] == 4 && hdr[7] == 32 &&
                    std::fread(f.clut, 1, sizeof f.clut, in) == sizeof f.clut &&
                    std::fread(f.present, 1, sizeof f.present, in) == sizeof f.present &&
                    std::fread(f.pixels.data(), 1, f.pixels.size(), in) == f.pixels.size();
    std::fclose(in);
    f.ok = ok && f.present[0];  // the space glyph stands in for characters without one
    if (!f.ok) std::fprintf(stderr, "[bigname] %s has a bad header; ASCII names stay blank\n", path.c_str());
    return f;
}

// --- names ---

bool sjis_lead(uint8_t c) { return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC); }

struct Name {
    bool sjis = true;   // only 2-byte Shift-JIS characters: the JP loader can draw it
    std::string ascii;  // otherwise: the characters to draw with the US font (at most 12)
};

/// A full-width Shift-JIS character as ASCII (space, digits, letters, long dash), else ' '.
char fullwidth_ascii(uint16_t c) {
    if (c >= 0x824F && c <= 0x8258) return static_cast<char>('0' + (c - 0x824F));
    if (c >= 0x8260 && c <= 0x8279) return static_cast<char>('A' + (c - 0x8260));
    if (c >= 0x8281 && c <= 0x829A) return static_cast<char>('a' + (c - 0x8281));
    if (c == 0x815B || c == 0x817C || c == 0x815D) return '-';
    return ' ';
}

Name read_name(PsxContext& ctx, uint32_t addr) {
    Name n;
    std::vector<uint8_t> s;
    for (uint32_t i = 0; i < 64; ++i) {
        const uint8_t c = psx_read8(&ctx, addr + i);
        if (c == 0) break;
        s.push_back(c);
    }
    for (size_t i = 0; i < s.size(); i += 2)
        if (!sjis_lead(s[i]) || i + 1 >= s.size()) {
            n.sjis = false;
            break;
        }
    if (n.sjis) return n;
    for (size_t i = 0; i < s.size() && n.ascii.size() < kMaxAscii;) {
        if (sjis_lead(s[i]) && i + 1 < s.size()) {
            n.ascii.push_back(fullwidth_ascii(static_cast<uint16_t>(s[i] << 8 | s[i + 1])));
            i += 2;
        } else {
            if (s[i] >= 0x20 && s[i] < 0x7F) n.ascii.push_back(static_cast<char>(s[i]));  // DEK tag bytes < 0x20 dropped
            ++i;
        }
    }
    return n;
}

bool trace() { return std::getenv("DCB_TRACE_TEXT") != nullptr; }

/// The big-name width (px) of the name at `addr`: the JP formula for Shift-JIS names, 16 per
/// character for ASCII (0 without the font: nothing was uploaded).
int16_t name_width(PsxContext& ctx, uint32_t addr) {
    const Name n = read_name(ctx, addr);
    if (!n.sjis) return static_cast<int16_t>(font().ok ? n.ascii.size() * kGlyphW : 0);
    ctx.r[kA0] = addr;
    f_8006BD60(&ctx);
    const int32_t len = static_cast<int32_t>(ctx.r[kV0]);
    return static_cast<int16_t>((len / 2) * 32);
}

/// Spawn a child task `entry(args...)` of the current task, the way the JP code does:
/// task_spawn(0, -1, 0, 0x800) with the entry and its arguments at sp+16.
void spawn(PsxContext& ctx, uint32_t frame, uint32_t entry, std::initializer_list<uint32_t> args) {
    wr32(ctx, frame + 16, entry);
    uint32_t off = 20;
    for (uint32_t a : args) {
        wr32(ctx, frame + off, a);
        off += 4;
    }
    ctx.r[kA0] = 0;
    ctx.r[kA1] = 0xFFFFFFFFu;
    ctx.r[kA2] = 0;
    ctx.r[kA3] = 0x800;
    f_80014AEC(&ctx);
}

/// task_sleep(frames) on the current task's stack; returns the wake-up value.
uint32_t sleep(PsxContext& ctx, uint32_t frame, uint32_t frames) {
    ctx.r[kA0] = frames;
    dcb_task_sleep(&ctx);
    const uint32_t v = ctx.r[kV0];
    ctx.r[kSp] = frame;
    return v;
}

void task_id(PsxContext& ctx) { f_800149E4(&ctx); }

/// The deck owner names of DECK2.DEK (the English one in assets/<serial>/files/B; JP stride 104,
/// owner at +73, 21 bytes), by deck number; empty when there is none.
const std::vector<std::string>& deck_owners() {
    static const std::vector<std::string> owners = [] {
        std::vector<std::string> out;
        const std::string path = dcb::asset_path("files/B/DECK2.DEK");
        FILE* in = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");
        if (!in) return out;
        std::vector<uint8_t> dek(8 + 159 * 104);
        const bool ok = std::fread(dek.data(), 1, dek.size(), in) == dek.size();
        std::fclose(in);
        for (size_t i = 0; ok && i < 159; ++i) {
            const char* owner = reinterpret_cast<const char*>(dek.data() + 8 + i * 104 + 73);
            out.emplace_back(owner, strnlen(owner, 21));
        }
        return out;
    }();
    return owners;
}

/// A name picture as the US MATCH archives hold one: a 4-bpp TIM at (704, 480), 16 px per
/// character from the US big font, its CLUT at (752, 472).
std::vector<uint8_t> name_picture(const std::string& name) {
    const BigFont& f = font();
    const size_t n = std::min<size_t>(name.size(), 16);  // the US pictures are at most 64 halfwords
    const uint32_t w = static_cast<uint32_t>(n) * 4, rows = 32, pixels = w * 2 * rows;
    std::vector<uint8_t> tim;
    const auto u16 = [&](uint32_t v) { tim.push_back(static_cast<uint8_t>(v)); tim.push_back(static_cast<uint8_t>(v >> 8)); };
    const auto u32 = [&](uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); };
    u32(0x10);
    u32(8);  // 4 bpp with a CLUT
    u32(12 + 32);
    for (const uint32_t v : {752u, 472u, 16u, 1u}) u16(v);
    tim.insert(tim.end(), f.clut, f.clut + 32);
    u32(12 + pixels);
    for (const uint32_t v : {704u, 480u, w, rows}) u16(v);
    for (uint32_t y = 0; y < rows; ++y) {
        for (size_t i = 0; i < n; ++i) {
            const int c = static_cast<uint8_t>(name[i]);
            const int g = c >= kFirst && c < kFirst + kCount && f.present[c - kFirst] ? c - kFirst : 0;
            const uint8_t* row = f.pixels.data() + static_cast<size_t>(g) * kGlyphBytes + y * 8;
            tim.insert(tim.end(), row, row + 8);
        }
    }
    return tim;
}

}  // namespace

bool dcb::vs_name_picture(const std::string& key, std::vector<uint8_t>& arc) {
    // B/MATCH/NNN.ARC: NNN is the opponent's deck; its owner's name, with the character names
    // swapped ([text] names), replaces the US picture when the swap changed it.
    if (key.rfind("B/MATCH/", 0) != 0 || key.size() != 15 || !font().ok) return false;
    const int deck = std::atoi(key.c_str() + 8);
    const std::vector<std::string>& owners = deck_owners();
    if (deck < 0 || static_cast<size_t>(deck) >= owners.size()) return false;
    std::string name = owners[static_cast<size_t>(deck)];
    dcb::text_swap_names(name);
    if (name == owners[static_cast<size_t>(deck)] || arc.size() < 8) return false;
    // The archive: u32 offsets (count = first / 4), the last one the end of the file.
    const uint32_t first = arc[0] | arc[1] << 8 | arc[2] << 16 | static_cast<uint32_t>(arc[3]) << 24;
    if (first % 4 != 0 || first < 8 || first > arc.size()) return false;
    const size_t count = first / 4;
    std::vector<uint32_t> offs(count);
    for (size_t i = 0; i < count; ++i)
        offs[i] = arc[i * 4] | arc[i * 4 + 1] << 8 | arc[i * 4 + 2] << 16 | static_cast<uint32_t>(arc[i * 4 + 3]) << 24;
    if (offs.back() != arc.size()) return false;
    const std::vector<uint8_t> picture = name_picture(name);
    std::vector<uint8_t> out(arc.begin(), arc.begin() + offs[count - 2]);  // up to the last TIM
    out.insert(out.end(), picture.begin(), picture.end());
    const uint32_t end = static_cast<uint32_t>(out.size());
    for (int i = 0; i < 4; ++i) out[(count - 1) * 4 + i] = static_cast<uint8_t>(end >> (8 * i));
    arc = std::move(out);
    return true;
}

extern "C" {

// 80044684 (task): bigname_load(name, row, parent). ASCII names with the US font; Shift-JIS to
// the original.
void dcb_bigname_load(PsxContext* ctx) {
    const uint32_t name = ctx->r[kA0], row = ctx->r[kA1], parent = ctx->r[kA2];
    const Name n = read_name(*ctx, name);
    if (n.sjis) return psx_call_original(ctx, kLoadTask);

    const uint32_t ra = ctx->r[kRa], sp = ctx->r[kSp];
    const BigFont& f = font();
    if (trace())
        std::printf("[bigname] row %u ascii \"%s\"%s\n", row, n.ascii.c_str(), f.ok ? "" : " (no en_bigfont.bin)");
    wr32(*ctx, kBignameBusy, 1);
    if (f.ok) {
        // One 320-byte TIM per glyph (4 bpp + CLUT) on this task's stack, uploaded like the
        // JP font TIMs: tim_upload(tim, x, y, 752, 471 + row), then DrawSync(0).
        const uint32_t frame = sp - 0x180, tim = frame + 0x20;
        wr32(*ctx, tim + 0, 0x10);
        wr32(*ctx, tim + 4, 8);
        wr32(*ctx, tim + 8, 12 + 32);
        wr32(*ctx, tim + 12, 0);                  // CLUT x, y: replaced by the upload
        wr32(*ctx, tim + 16, 16 | (1u << 16));    // 16 x 1
        for (uint32_t i = 0; i < 32; ++i) psx_write8(ctx, tim + 20 + i, f.clut[i]);
        wr32(*ctx, tim + 52, 12 + kGlyphBytes);
        wr32(*ctx, tim + 56, 0);                  // image x, y: replaced by the upload
        wr32(*ctx, tim + 60, 4 | (32u << 16));    // 4 halfwords x 32 rows
        for (size_t i = 0; i < n.ascii.size(); ++i) {
            int g = static_cast<uint8_t>(n.ascii[i]) - kFirst;
            if (g < 0 || g >= kCount || !f.present[g]) g = 0;  // no glyph: a space
            const uint8_t* px = f.pixels.data() + static_cast<size_t>(g) * kGlyphBytes;
            for (uint32_t b = 0; b < kGlyphBytes; ++b) psx_write8(ctx, tim + 64 + b, px[b]);
            ctx->r[kSp] = frame;
            wr32(*ctx, frame + 16, kClutY + row);
            ctx->r[kA0] = tim;
            ctx->r[kA1] = static_cast<uint32_t>(kVramX + static_cast<int>(i) * 4);
            ctx->r[kA2] = static_cast<uint32_t>(kVramY + static_cast<int>(row) * 32);
            ctx->r[kA3] = kClutX;
            f_8001B67C(ctx);
            ctx->r[kA0] = 0;
            f_8006740C(ctx);
        }
        ctx->r[kSp] = sp;
    }
    wr32(*ctx, kBignameBusy, 0);
    // task_wake(parent, 1): vs_names_load drops the value (the original passes a leftover a1).
    ctx->r[kA0] = parent;
    ctx->r[kA1] = 1;
    f_80014B7C(ctx);
    ctx->r[kSp] = sp;
    ctx->r[kRa] = ra;
}

// KAWSEG 801F04FC (task): vs_names_load(mode, match, parent).
void dcb_vs_names_load(PsxContext* ctx) {
    const uint32_t mode = ctx->r[kA0], parent = ctx->r[kA2];
    uint32_t match = ctx->r[kA1];
    const uint32_t ra = ctx->r[kRa], sp = ctx->r[kSp];
    const uint32_t frame = sp - 136, path = frame + 40;
    ctx->r[kSp] = frame;
    wr32(*ctx, kVsBusy, 1);
    uint32_t names = 1;
    if (mode == 0) {
        match = 999;
        names = 2;
    }
    for (uint32_t i = 0; i < names; ++i) {
        task_id(*ctx);
        const uint32_t self = ctx->r[kV0];
        spawn(*ctx, frame, kLoadTask, {rd32(*ctx, kPlayerRecords) + i * kRecordStride, i, self, 0});
        sleep(*ctx, frame, 0x7FFFFFFFu);
    }

    ctx->r[kA0] = path;
    ctx->r[kA1] = kMatchFormat;
    ctx->r[kA2] = match;
    f_8006C7B0(ctx);
    task_id(*ctx);
    const uint32_t self = ctx->r[kV0];
    spawn(*ctx, frame, kFileLoadTask, {path, self});
    const uint32_t arc = sleep(*ctx, frame, 0x7FFFFFFFu);
    if (arc) {
        // Every entry of the offset table, the end-of-file one included (as the original).
        for (uint32_t i = 0; i < (rd32(*ctx, arc) >> 2); ++i) {
            wr32(*ctx, frame + 16, 0xFFFFFFFFu);
            ctx->r[kA0] = arc + rd32(*ctx, arc + i * 4);
            ctx->r[kA1] = ctx->r[kA2] = ctx->r[kA3] = 0xFFFFFFFFu;
            f_8001B67C(ctx);
            sleep(*ctx, frame, rd32(*ctx, kFrameWait));
            ctx->r[kA0] = 0;
            f_8006740C(ctx);
        }
    }
    ctx->r[kA0] = arc;
    f_8001B0D4(ctx);

    const uint32_t p0 = rd32(*ctx, kBattlePlayers), p1 = rd32(*ctx, kBattlePlayers + 4);
    const int16_t w0 = name_width(*ctx, p0 + kNameOff);
    int32_t w1;
    if (mode != 0)
        w1 = static_cast<int16_t>(psx_read16(ctx, rd32(*ctx, kTimPrect) + 4)) * 4;  // the name picture
    else
        w1 = name_width(*ctx, p1 + kNameOff);
    psx_write16(ctx, p0 + kWidthOff, static_cast<uint16_t>(w0));
    psx_write16(ctx, p1 + kWidthOff, static_cast<uint16_t>(w1));
    if (trace())
        std::printf("[bigname] widths %d %d (mode %u, match %u; names at %08X %08X, records at %08X)\n", w0, w1,
                    mode, match, p0 + kNameOff, p1 + kNameOff, rd32(*ctx, kPlayerRecords));

    // The four slide-in records (x, y, ..., dx, dy, ...) exactly as 801F0718..801F07D4 set them.
    const uint32_t a = kVsAnim;
    const int32_t first[8] = {100, 241, 184, 121, 76, -113, 8, 7};
    for (uint32_t k = 0; k < 8; ++k) wr32(*ctx, a + k * 4, static_cast<uint32_t>(first[k]));
    wr32(*ctx, a + 0x20, 320);
    wr32(*ctx, a + 0x24, 195);
    wr32(*ctx, a + 0x30, static_cast<uint32_t>(-w1));
    wr32(*ctx, a + 0x34, 16);
    wr32(*ctx, a + 0x38, static_cast<uint32_t>(312 - w1));
    wr32(*ctx, a + 0x40, 320);
    wr32(*ctx, a + 0x44, 159);
    wr32(*ctx, a + 0x50, static_cast<uint32_t>(-136));
    wr32(*ctx, a + 0x54, 66);
    wr32(*ctx, a + 0x60, 320);
    wr32(*ctx, a + 0x64, 177);
    wr32(*ctx, a + 0x70, static_cast<uint32_t>(-136));
    wr32(*ctx, a + 0x74, 48);

    sleep(*ctx, frame, 10);
    wr32(*ctx, kVsBusy, 0);
    ctx->r[kA0] = parent;
    ctx->r[kA1] = static_cast<uint32_t>(-136);  // what the original's a1 last held
    f_80014B7C(ctx);
    ctx->r[kSp] = sp;
    ctx->r[kRa] = ra;
}

}  // extern "C"
