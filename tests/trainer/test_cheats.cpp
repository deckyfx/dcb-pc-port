// Cheat codes: line parsing, every supported code type's effect on a RAM buffer, conditionals,
// the serial repeater, rejected codes, and the editable cheat file (toggle / add / remove keep
// the user's comments).

#include "settings.hpp"  // read_text_file
#include "trainer_cheats.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

namespace {

using namespace trainer;

std::vector<uint8_t> ram() { return std::vector<uint8_t>(kRamSize, 0); }

uint16_t rd16(const std::vector<uint8_t>& r, uint32_t off) { return static_cast<uint16_t>(r[off] | (r[off + 1] << 8)); }

std::vector<CodeOp> must_compile(const std::vector<std::string>& lines) {
    std::vector<CodeOp> ops;
    std::string error;
    if (!compile_codes(lines, ops, error)) {
        std::fprintf(stderr, "compile failed: %s\n", error.c_str());
        std::exit(1);
    }
    return ops;
}

std::string compile_error(const std::vector<std::string>& lines) {
    std::vector<CodeOp> ops;
    std::string error;
    CHECK(!compile_codes(lines, ops, error));
    CHECK(!error.empty());
    return error;
}

ApplyStats run(std::vector<uint8_t>& r, const std::vector<std::string>& lines) {
    return apply_codes(must_compile(lines), r.data());
}

void test_parse_line() {
    uint32_t code = 0;
    uint16_t value = 0;
    std::string error;
    CHECK(parse_code_line("800B1234 270F", code, value, error));
    CHECK(code == 0x800B1234 && value == 0x270F);
    CHECK(parse_code_line("  300b1236\t00ff  ; money ", code, value, error));
    CHECK(code == 0x300B1236 && value == 0x00FF);
    CHECK(parse_code_line("800B1234 270F # comment", code, value, error));
    CHECK(!parse_code_line("800B1234", code, value, error));
    CHECK(!parse_code_line("800B123 270F", code, value, error));
    CHECK(!parse_code_line("800B1234 270", code, value, error));
    CHECK(!parse_code_line("800B12G4 270F", code, value, error));
    CHECK(!parse_code_line("800B1234 270F 1", code, value, error));
    CHECK(!parse_code_line("", code, value, error));
}

void test_writes() {
    auto r = ram();
    ApplyStats s = run(r, {"80001000 BEEF"});
    CHECK(rd16(r, 0x1000) == 0xBEEF);
    CHECK(s.writes == 1 && s.bytes_changed == 2);
    s = run(r, {"80001000 BEEF"});  // already there: written, nothing changes
    CHECK(s.writes == 1 && s.bytes_changed == 0);
    run(r, {"30001003 0042"});
    CHECK(r[0x1003] == 0x42 && r[0x1002] == 0);
    // Last byte of RAM is writable with an 8-bit code, top of RAM with a 16-bit one.
    run(r, {"301FFFFF 0001", "801FFFFD 1234"});
    CHECK(r[0x1FFFFF] == 0x01 && rd16(r, 0x1FFFFD) == 0x1234);
}

void test_increment_decrement() {
    auto r = ram();
    r[0x10] = 0xFE;
    r[0x11] = 0xFF;  // 0xFFFE
    run(r, {"10000010 0003"});
    CHECK(rd16(r, 0x10) == 0x0001);  // wraps
    run(r, {"11000010 0002"});
    CHECK(rd16(r, 0x10) == 0xFFFF);
    r[0x20] = 0xFF;
    run(r, {"20000020 0002"});
    CHECK(r[0x20] == 0x01 && r[0x21] == 0);  // 8-bit wrap, neighbour untouched
    run(r, {"21000020 0005"});
    CHECK(r[0x20] == 0xFC);
}

void test_conditionals() {
    auto r = ram();
    r[0x100] = 0x34;
    r[0x101] = 0x12;  // 16-bit 0x1234 at 0x100, 8-bit 0x34 at 0x100
    struct Case {
        const char* cond;
        bool expect;
    };
    const Case cases[] = {
        {"D0000100 1234", true},  {"D0000100 1235", false}, {"D1000100 1235", true}, {"D1000100 1234", false},
        {"D2000100 1235", true},  {"D2000100 1234", false}, {"D3000100 1233", true}, {"D3000100 1234", false},
        {"E0000100 0034", true},  {"E0000100 0035", false}, {"E1000100 0033", true}, {"E1000100 0034", false},
        {"E2000100 0035", true},  {"E2000100 0034", false}, {"E3000100 0033", true}, {"E3000100 0034", false},
    };
    for (const Case& c : cases) {
        r[0x200] = 0;
        r[0x201] = 0;
        run(r, {c.cond, "30000200 0001", "30000201 0001"});
        if (r[0x200] != (c.expect ? 1 : 0) || r[0x201] != 1) {
            std::fprintf(stderr, "condition %s: got %d/%d\n", c.cond, r[0x200], r[0x201]);
            std::exit(1);
        }
    }
    // A chain of conditions guards one code; all must hold.
    r[0x200] = 0;
    run(r, {"D0000100 1234", "E0000100 0034", "30000200 0007"});
    CHECK(r[0x200] == 7);
    r[0x200] = 0;
    run(r, {"D0000100 1234", "E0000100 0099", "30000200 0007", "30000201 0009"});
    CHECK(r[0x200] == 0 && r[0x201] == 9);
    r[0x200] = 0;
    run(r, {"D0000100 9999", "E0000100 0034", "30000200 0007", "30000201 000A"});
    CHECK(r[0x200] == 0 && r[0x201] == 0x0A);
    // A false condition at the end of a cheat is harmless.
    run(r, {"30000202 0001", "D0000100 0000"});
    CHECK(r[0x202] == 1);
    // C0 gates the rest of the cheat.
    r[0x300] = r[0x301] = 0;
    run(r, {"C0000100 1234", "30000300 0001", "30000301 0001"});
    CHECK(r[0x300] == 1 && r[0x301] == 1);
    r[0x300] = r[0x301] = 0;
    run(r, {"30000302 0005", "C0000100 0000", "30000300 0001", "30000301 0001"});
    CHECK(r[0x300] == 0 && r[0x301] == 0 && r[0x302] == 5);
    // A condition in front of a repeater skips the whole repeater.
    r[0x400] = 0;
    run(r, {"D0000100 0000", "50000302 0000", "30000400 0001"});
    CHECK(r[0x400] == 0);
}

void test_repeater() {
    auto r = ram();
    // 4 x 16-bit writes, address +4, value +0x10.
    const ApplyStats s = run(r, {"50000404 0010", "80002000 0100"});
    CHECK(s.writes == 4);
    CHECK(rd16(r, 0x2000) == 0x0100 && rd16(r, 0x2004) == 0x0110 && rd16(r, 0x2008) == 0x0120 &&
          rd16(r, 0x200C) == 0x0130);
    CHECK(rd16(r, 0x2010) == 0 && rd16(r, 0x2002) == 0);
    // 8-bit: 3 bytes, address +1, value +1.
    run(r, {"50000301 0001", "30003000 0041"});
    CHECK(r[0x3000] == 0x41 && r[0x3001] == 0x42 && r[0x3002] == 0x43 && r[0x3003] == 0);
    // Count 0 writes nothing.
    CHECK(run(r, {"50000001 0000", "30003100 0001"}).writes == 0);
    CHECK(r[0x3100] == 0);
}

void test_rejected() {
    CHECK(compile_error({}) == "no codes");
    CHECK(compile_error({"C1000000 0010"}).find("C1") != std::string::npos);
    CHECK(compile_error({"C2001000 0004", "80002000 0000"}).find("copy") != std::string::npos);
    CHECK(compile_error({"D4000000 0001"}).find("pad") != std::string::npos);
    CHECK(compile_error({"1F800000 0001"}).find("not supported") != std::string::npos);
    CHECK(compile_error({"90001000 0001"}).find("90") != std::string::npos);
    CHECK(compile_error({"80200000 0001"}).find("outside") != std::string::npos);  // past 2 MB
    CHECK(compile_error({"801FFFFF 0001"}).find("16-bit") != std::string::npos);
    CHECK(compile_error({"30001000 0100"}).find("00vv") != std::string::npos);
    CHECK(compile_error({"50000402 0001"}).find("following") != std::string::npos);
    CHECK(compile_error({"50000402 0001", "10001000 0001"}).find("80 or 30") != std::string::npos);
    CHECK(compile_error({"50010402 0001", "80001000 0001"}).find("5000nnss") != std::string::npos);
    CHECK(compile_error({"5000FF20 0000", "801FF000 0000"}).find("past the end") != std::string::npos);
    CHECK(compile_error({"80001000 0001", "garbage"}).find("not a code") != std::string::npos);
}

const char* const kFile =
    "# DCB cheats\n"
    "; second comment style\n"
    "[Infinite money] on\n"
    "80001000 270F  ; 9999\n"
    "\n"
    "# about max cards\n"
    "[Max cards]\n"
    "30001100 0063\n"
    "[Broken] on\n"
    "C2001000 0004\n"
    "80002000 0000\n"
    "[Empty] off\n";

void test_cheat_set() {
    CheatSet set = CheatSet::parse(kFile);
    CHECK(set.cheats().size() == 4);
    const Cheat& money = set.cheats()[0];
    CHECK(money.name == "Infinite money" && money.enabled && money.error.empty() && money.codes.size() == 1);
    CHECK(money.codes[0] == "80001000 270F");
    CHECK(!set.cheats()[1].enabled && set.cheats()[1].error.empty());
    CHECK(!set.cheats()[2].enabled && !set.cheats()[2].error.empty());  // rejected: never applied
    CHECK(set.cheats()[3].error == "no codes");
    CHECK(set.warnings().size() == 2);
    CHECK(set.enabled_count() == 1);
    CHECK(set.text() == kFile);  // untouched round trip

    auto r = ram();
    ApplyStats s = set.apply(r.data());
    CHECK(rd16(r, 0x1000) == 9999 && r[0x1100] == 0 && rd16(r, 0x2000) == 0);
    CHECK(s.writes == 1);

    // Toggle: rewrites the header line only.
    CHECK(set.set_enabled(1, true));
    CHECK(!set.set_enabled(2, true));  // invalid cheats cannot be enabled
    CHECK(!set.set_enabled(9, true));
    CHECK(set.set_enabled(0, false));
    CHECK(set.text().find("[Max cards] on\n30001100 0063\n") != std::string::npos);
    CHECK(set.text().find("[Infinite money] off\n80001000 270F  ; 9999\n") != std::string::npos);
    CHECK(set.text().find("# about max cards\n") != std::string::npos);
    r = ram();
    set.apply(r.data());
    CHECK(r[0x1100] == 0x63 && rd16(r, 0x1000) == 0);
    // The edited text parses back to the same state.
    const CheatSet again = CheatSet::parse(set.text());
    CHECK(again.cheats().size() == 4 && !again.cheats()[0].enabled && again.cheats()[1].enabled);

    // Add, then remove: comments of the neighbours survive.
    std::string error;
    CHECK(!set.add("Bad", {"C1000000 0000"}, true, &error) && !error.empty());
    CHECK(set.add("Freeze [x]", freeze_codes(0x1200, 0xABCD, 2), true));
    CHECK(set.cheats().size() == 5 && set.cheats()[4].name == "Freeze  x" && set.cheats()[4].enabled);
    CHECK(set.remove(0));
    CHECK(set.cheats().size() == 4 && set.cheats()[0].name == "Max cards");
    CHECK(set.text().rfind("# DCB cheats\n; second comment style\n# about max cards\n[Max cards] on\n", 0) == 0);
    CHECK(!set.remove(10));
}

void test_codes_before_header_and_bad_state() {
    const CheatSet set = CheatSet::parse("80001000 0001\r\n[A] maybe\r\n80001000 0002\r\n[B\r\n30001000 0001\r\n");
    CHECK(set.cheats().size() == 2);
    CHECK(set.cheats()[0].name == "A" && !set.cheats()[0].enabled);
    CHECK(set.cheats()[1].name == "B");
    CHECK(set.warnings().size() == 3);
}

void test_freeze_codes() {
    CHECK(freeze_codes(0x0B1234, 0x7F, 1) == std::vector<std::string>{"300B1234 007F"});
    CHECK(freeze_codes(0x0B1234, 0x270F, 2) == std::vector<std::string>{"800B1234 270F"});
    CHECK((freeze_codes(0x0B1234, 0x12345678, 4) == std::vector<std::string>{"800B1234 5678", "800B1236 1234"}));
    // Freeze codes compile and do what they say.
    auto r = ram();
    run(r, freeze_codes(0x40, 0xDEADBEEF, 4));
    CHECK(rd16(r, 0x40) == 0xBEEF && rd16(r, 0x42) == 0xDEAD);
}

void test_resolve_path() {
    namespace fs = std::filesystem;
    const fs::path cwd = "/game", exe = "/opt/dcb";
    const auto none = [](const fs::path&) { return false; };
    const auto only_exe = [&](const fs::path& p) { return p == exe / "cheats" / "SLPS-03101.txt"; };
    const auto both = [](const fs::path&) { return true; };
    CHECK(resolve_cheat_path("SLPS-03101", "/x/my.txt", cwd, exe, both) == fs::path("/x/my.txt"));
    CHECK(resolve_cheat_path("SLPS-03101", "", cwd, exe, both) == cwd / "cheats" / "SLPS-03101.txt");
    CHECK(resolve_cheat_path("SLPS-03101", "", cwd, exe, only_exe) == exe / "cheats" / "SLPS-03101.txt");
    CHECK(resolve_cheat_path("SLPS-03101", "", cwd, exe, none) == cwd / "cheats" / "SLPS-03101.txt");
}

/// The shipped template must parse cleanly with everything off.
void test_example_file() {
    const std::optional<std::string> text = platform::read_text_file(DCB_CHEATS_EXAMPLE);
    CHECK(text.has_value());
    const CheatSet set = CheatSet::parse(*text);
    for (const std::string& w : set.warnings()) std::fprintf(stderr, "example: %s\n", w.c_str());
    CHECK(set.warnings().empty());
    CHECK(set.cheats().size() == 3);
    CHECK(set.enabled_count() == 0);
}

}  // namespace

int main() {
    test_parse_line();
    test_writes();
    test_increment_decrement();
    test_conditionals();
    test_repeater();
    test_rejected();
    test_cheat_set();
    test_codes_before_header_and_bad_state();
    test_freeze_codes();
    test_resolve_path();
    test_example_file();
    std::puts("trainer cheats: all tests passed");
    return 0;
}
