// English-text builder pieces (src/patch/text_*.cpp) on synthetic data, no game data needed:
// VCDIFF and .xz/LZMA2 decoding, MSD walk and program comparison, catalog templates and rows,
// the fix porter, the report's bytes repr, PAK / ARC round trips. The whole pipeline is checked
// against tools/text/en_text.py on real dumps by tools/patch/compare_text.sh.

#include "patch/patch.hpp"
#include "patch/text_internal.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

namespace {

using patch::Bytes;
using patch::View;
using namespace patch::text;

Bytes hex(std::string_view h) {
    Bytes out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoul(std::string(h.substr(i, 2)), nullptr, 16)));
    return out;
}

Bytes bytes(std::string_view s) { return Bytes(s.begin(), s.end()); }

bool same(View a, std::string_view b) { return patch::equal(a, patch::view(b)); }

// ---------------------------------------------------------------- VCDIFF / xz

// Source "ABCDEFGHIJKLMNOP"; target "ABCDEFGH" + ADD "xyz" + RUN 4 '-' + COPY 6 overlapping the
// target (here - 2) + "MNOP" by address, by the near cache and by the same cache. Checked with
// tools/text/vcdiff.py.
constexpr std::string_view kPatch = "d6c3c4000001100016210004080578797a2d180400042614547400020c000c";
// The same with the data section as an xz stream (xdelta3's LZMA secondary compression, id 2).
constexpr std::string_view kPatchLzma =
    "d6c3c40001020110004b210139080504fd377a585a0000016922de360200210116000000742fe5a301000378797a2d00b9c9e0"
    "44000118046be9f0a59042990d010000000001595a180400042614547400020c000c";
constexpr std::string_view kTarget = "ABCDEFGHxyz----------MNOPMNOPMNOP";

void test_vcdiff() {
    const Bytes source = bytes("ABCDEFGHIJKLMNOP");
    CHECK(same(vcdiff_decode(source, hex(kPatch)), kTarget));
    CHECK(same(vcdiff_decode(source, hex(kPatchLzma)), kTarget));
    bool threw = false;
    try {
        vcdiff_decode(source, bytes("not a patch"));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    Bytes truncated = hex(kPatch);
    truncated.pop_back();
    threw = false;
    try {
        vcdiff_decode(source, truncated);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

std::string xz_sample() {
    std::string s;
    for (int i = 0; i < 20; ++i) s += "The quick brown fox jumps over the lazy dog. ";
    for (int i = 0; i < 256; ++i) s += static_cast<char>(i);
    return s;
}

void test_xz() {
    // Python: lzma.compress(xz_sample(), format=FORMAT_XZ, check=CHECK_CRC64, preset=6)
    const Bytes crc64 = hex(
        "fd377a585a000004e6d6b4460200210116000000742fe5a3e0048301185d002a1a08a2032566f14b78c5a205ff2ee6d9d220"
        "1aad34f8e21de84136fadc0669bb3ce410342709ebb366e3ed3798ed92add5273cc810c1f3af57b7aca09395ce2938b00dda"
        "28219685e9c2dca6ed35197d1e601208f38f5a6ff4595e4a04ee2bbb122b2039adbc2f6da17f09f08f89f40afd1ad766a890"
        "cf142b8cbb67e34bd95b9a98cac610c3147460e21a201d531aa2555f6a5dc5e30dd616104b4de8fad0cad24dd5bf1c07d18a"
        "873ebda412b3bb67a15e6f639deb91fb0f809df2bfc2b198ab80dfe1bbe41e0d9b23cd58f0be5b9d86b102f7bf94853a99a9"
        "cc7c87fca1ee8c1462e82b71325135eab69d194dc141c09c2c0100eb605cf35eb08a9b81158718dfef58001ea50a24411245"
        "d38c8d116026531b99070600f65859a34583593e0001b402840900003dcc2455b1c467fb020000000004595a");
    // Python: the same with check=CHECK_NONE and LZMA2 lc=1 lp=2 pb=1 (other literal / position
    // contexts).
    const Bytes lclppb = hex(
        "fd377a585a000000ff12d9410200210116000000742fe5a3e00483012f40002a1a0907249edbdc6a0de0855907d5dc21b603"
        "f50b77f3d9aee79ab01b1ea243234d7377821dd5383ac772f7736abd2b4e723be8e6a68d99bba1f7b57192df308ea8737da5"
        "7d403420449e64b6c1aea1821f3f0fea043c7139162de245974b2f0fe0ea0ce030368a716b850ad81995001827ac6a7a1c13"
        "8ed5a315807ffab29fe12342dd725799fd0ff024e9d8b87f75a288735d6c331fa25edbc1ae6ac8d447305099153dbe9118ee"
        "534245668e94b5545691e022a4ac08e1d9ddac9429a4c5a95f99035ec645d6d5b8d2b6b9c5196a9e2def3f58d36ef5a4910f"
        "0eec3ac8cbf520c2aa01861462ce598b1e46cb4c2b8c6f25c108f91e73e7489587aa5f5d381a99c6b5ba260621a5d385234d"
        "b98c95f0fc7db98086883b119fdf7c13c09f5d912656ef5492a65f496e6d3682b66a00000001c3028409000044f10343a800"
        "0afc020000000000595a");
    const std::string want = xz_sample();
    CHECK(same(xz_decode(crc64, 1 << 20), want));
    CHECK(same(xz_decode(lclppb, 1 << 20), want));
    CHECK(same(xz_decode(crc64, 10), want.substr(0, 10)));  // stops at the size asked for
    Bytes broken = crc64;
    broken[40] ^= 0x55;
    bool threw = false;
    try {
        threw = !same(xz_decode(broken, 1 << 20), want);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

// ---------------------------------------------------------------- MSD

void put16(Bytes& b, uint16_t v) { patch::wr16(b, v); }

/// A text record: op 8, register, length (NUL included), text, padded to 4.
void text_rec(Bytes& s, uint16_t reg, std::string_view t) {
    put16(s, 8);
    put16(s, reg);
    put16(s, static_cast<uint16_t>(t.size() + 1));
    s.insert(s.end(), t.begin(), t.end());
    s.push_back(0);
    while (s.size() % 4) s.push_back(0);
}

void rec(Bytes& s, std::initializer_list<uint16_t> halves) {
    for (uint16_t h : halves) put16(s, h);
}

Bytes script(bool us) {
    Bytes s = bytes("MSCD");
    s.resize(16, 0);
    text_rec(s, 1, us ? "Push *b1 to go to map" : "\x83\x7d\x83\x62\x83\x76");
    if (us) rec(s, {0x0A, 4});                            // show-text command: text-side
    rec(s, {5, 0, us ? uint16_t{0x40} : uint16_t{0x30}, 0});  // jump: target moves with the text
    rec(s, {9, 4, 1, 2, 3, 4});                           // conditional skip on register 4
    rec(s, {0x0B, 7, 0, 1});                              // host command 7
    return s;
}

void test_msd() {
    const Bytes jp = script(false), us = script(true);
    const auto recs = msd_walk(us);
    CHECK(recs.size() == 5);
    CHECK(recs[0].op == kMsdText && recs[0].has_text && same(recs[0].text, std::string_view("Push *b1 to go to map\0", 22)));
    CHECK(recs[1].op == 0x0A && recs[1].offset == 16 + 28);
    CHECK(msd_skeleton(us, city_show_text()).size() == 3);
    CHECK(msd_same_program(jp, us, city_show_text(), {}));

    Bytes other = us;
    other[other.size() - 2] = 9;  // the host command's argument
    CHECK(!msd_same_program(jp, other, city_show_text(), {}));
    // A test of one pad register may test another one at the same place.
    Bytes moved = us;
    moved[16 + 28 + 4 + 8 + 2] = 5;
    CHECK(!msd_same_program(jp, moved, city_show_text(), {}));
    CHECK(msd_same_program(jp, moved, city_show_text(), {4, 5}));

    // The city graft: the US script chunk (buttons remapped) in the JP PAK, other chunks JP.
    std::vector<patch::PakChunk> jp_pak{{5, 1, bytes("jp image")}, {2, 7, jp}};
    std::vector<patch::PakChunk> us_pak{{2, 7, us}, {5, 1, bytes("us image")}};
    const auto pak = graft_city_script(patch::write_pak(jp_pak), patch::write_pak(us_pak));
    CHECK(pak.has_value());
    const auto chunks = patch::read_pak(*pak);
    CHECK(chunks.size() == 2 && same(chunks[0].data, "jp image") && chunks[1].kind == 2);
    CHECK(msd_walk(chunks[1].data)[0].text[7] == '2');  // *b1 -> *b2
    CHECK(!graft_city_script(patch::write_pak(jp_pak), patch::write_pak({{2, 7, other}})).has_value());
}

// ---------------------------------------------------------------- catalog

void test_catalog() {
    CHECK(same(jp_template(bytes("\x83\x58\x83\x8D\x83\x62\x83\x67S\x82\xCC ??\x96\x87")),
               "\x83\x58\x83\x8D\x83\x62\x83\x67%c\x82\xCC %2d\x96\x87"));
    CHECK(same(jp_template(bytes("??x")), "??x"));
    CHECK(same(us_template(bytes("slot *S, *E *x")), "slot %c, %c *x"));
    CHECK(same(escape(bytes("a\\b\nc\td")), "a\\\\b\\nc\\td"));

    // "EXE": two strings (zero padding between), "SEG" (an overlay): the US side.
    const Bytes jp_exe = bytes(std::string_view("\x82\xCD\x82\xA2\0\0\0\0\x82\xA2\x82\xA2\x82\xA6\0", 15));
    const Bytes us_seg = bytes(std::string_view("Yes\0No\0", 7));
    const FileLookup jp = [&](const std::string& n) -> View {
        CHECK(n == "EXE");
        return jp_exe;
    };
    const FileLookup us = [&](const std::string& n) -> View {
        CHECK(n == "SEG");
        return us_seg;
    };
    std::map<std::string, Bytes> own;
    read_own(bytes("# comment\r\nEXE:8\tNo\\n(own)\n\n   \nbad line\n"), own);
    CHECK(own.size() == 2 && same(own["EXE:8"], "No\\n(own)") && own["bad line"].empty());
    Rows source, en;
    std::vector<std::string> problems;
    build_catalog("# header\nrun EXE:0 SEG:0x0 2  # two in a row\npair EXE:8 -\n", jp, us, own, source, en, problems);
    CHECK(source.size() == 3 && source[0].first == "EXE:0" && source[1].first == "EXE:8");
    CHECK(en.size() == 3 && same(en[0].second, "Yes") && same(en[1].second, "No\\n(own)"));
    CHECK(problems.empty());
    CHECK(same(write_rows(en, &own), "EXE:0\tYes\nEXE:8\tNo\\n(own)\nEXE:8\tNo\\n(own)\n"));
    CHECK(same(write_rows({{"A:1", bytes("x\ny")}}), "A:1\tx\\ny\n"));

    bool threw = false;
    try {
        build_catalog("pear EXE:0 SEG:0\n", jp, us, own, source, en, problems);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    source.clear();
    en.clear();
    build_catalog("pair EXE:0 -\n", jp, us, {}, source, en, problems);
    CHECK(en.empty() && problems.size() == 1);
}

// ---------------------------------------------------------------- fixes, report, containers

void test_port_fix() {
    const Bytes before = bytes("header..0123456789ABCDEF..tail--");
    Bytes after = before;
    after[12] = 'x';
    after[13] = 'y';
    // The target: the same data moved 3 bytes on, and a copy of unrelated text.
    const Bytes target = bytes("new_header..0123456789ABCDEF..tail--xx");
    std::vector<std::string> notes;
    const Bytes ported = port_fix(before, after, target, &notes);
    CHECK(same(ported, "new_header..0123xy6789ABCDEF..tail--xx"));
    CHECK(notes.size() == 1);
    // A run whose surroundings are not unique in the target is left alone.
    const Bytes twice = bytes("0123456789ABCDEF0123456789ABCDEF");
    const Bytes b2 = bytes("0123456789ABCDEF");
    Bytes a2 = b2;
    a2[8] = '!';
    CHECK(same(port_fix(b2, a2, twice, &notes), "0123456789ABCDEF0123456789ABCDEF"));
}

void test_repr() {
    CHECK(py_repr(bytes("plain")) == "b'plain'");
    CHECK(py_repr(bytes("it's")) == "b\"it's\"");
    CHECK(py_repr(bytes("'\"")) == "b'\\'\"'");
    CHECK(py_repr(bytes(std::string_view("a\\b\n\t\x01\x7f", 7))) == "b'a\\\\b\\n\\t\\x01\\x7f'");
}

void test_containers() {
    const std::vector<patch::PakChunk> chunks{{1, 2, bytes("abc")}, {7, 0x1234, {}}};
    const Bytes pak = patch::write_pak(chunks);
    CHECK(pak.size() == 8 + 3 + 8 + 4);
    const auto back = patch::read_pak(pak);
    CHECK(back.size() == 2 && back[1].id == 0x1234 && same(back[0].data, "abc"));
    Bytes bad = pak;
    bad.resize(bad.size() - 4);
    bad.push_back(1);
    bool threw = false;
    try {
        patch::read_pak(bad);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);

    const Bytes a = bytes("first"), b = bytes("second!");
    const Bytes arc = patch::write_arc({a, b});
    const auto entries = patch::arc_entries(arc);
    CHECK(entries && entries->size() == 2 && same((*entries)[1], "second!"));
    CHECK(!patch::arc_entries(bytes("\x03\0\0\0xxxx")).has_value());
}

void test_cancel() {
    // Cancelled at the first progress report: nothing is read or written.
    patch::Inputs in;
    in.jp_dump = "/nonexistent/jp";
    in.us_dump = "/nonexistent/us";
    in.assets = "/nonexistent/assets";
    int calls = 0;
    bool cancelled = false;
    try {
        patch::build_text(in, [&](const patch::Progress& p) {
            ++calls;
            CHECK(std::string(p.stage) == "English text" && p.total > 0);
            return false;
        });
    } catch (const patch::Cancelled&) {
        cancelled = true;
    }
    CHECK(cancelled && calls == 1);
}

}  // namespace

int main() {
    test_vcdiff();
    test_xz();
    test_msd();
    test_catalog();
    test_port_fix();
    test_repr();
    test_containers();
    test_cancel();
    std::puts("patch text: all checks passed");
    return 0;
}
