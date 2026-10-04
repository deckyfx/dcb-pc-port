#pragma once
// The pieces of patch::build_text (text.cpp), one per module of the Python pipeline it ports.
// Internal to src/patch; declared here so the unit tests (tests/patch) can reach the pure parts.
//
//   text_msd.cpp      tools/text/msd.py      MSD script records, same_program
//   text_scripts.cpp  tools/text/scripts.py  tutorial / Fusion Shop scripts; city script graft
//   text_vcdiff.cpp   tools/text/vcdiff.py   VCDIFF (xdelta3) decoder
//   text_xz.cpp       (Python's lzma)        .xz / LZMA2 decoder for xdelta3's secondary sections
//   text_fixes.cpp    tools/text/fixes.py    community fixes on the rebuilt reference image
//   text_image.cpp    (extract_disc layout)  raw disc image rebuilt from an imported dump
//   text_catalog.cpp  tools/text/catalog.py  text catalog -> source.tsv / en.tsv
//   text_bigfont.cpp  tools/text/bigfont.py  VS-screen big-name font and name pictures
//   text_cards.cpp    tools/text/en_text.py  font rows, CARD2.CDD / DECK2.DEK grafts

#include "containers.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace patch::text {

// ---------------------------------------------------------------- MSD (msd.py)

struct MsdRecord {
    size_t offset = 0;
    uint16_t op = 0;
    View raw;             ///< the whole record (text: the 6-byte header only)
    bool has_text = false;
    View text;            ///< op 8: the string (NUL included)
};
inline constexpr uint16_t kMsdText = 8;
inline constexpr uint16_t kMsdJump = 5;
/// (op, cmd) pairs of host commands that only show / page text.
using ShowText = std::set<std::pair<uint16_t, uint16_t>>;
/// Pad registers a host fills (a test of one may test another at the same place).
using ButtonRegs = std::set<uint16_t>;

/// Every record after the 16-byte header; throws on a missing magic or an unknown op.
std::vector<MsdRecord> msd_walk(View script);
bool msd_is_text_side(const MsdRecord& r, const ShowText& show_text);
bool msd_is_button_test(const MsdRecord& r, const ButtonRegs& regs);
/// The records that are not text-side.
std::vector<MsdRecord> msd_skeleton(View script, const ShowText& show_text);
/// msd.same_program: true when the scripts differ only in text and show-text commands.
bool msd_same_program(View jp, View us, const ShowText& show_text, const ButtonRegs& regs, std::string* why = nullptr);
/// The city host's show-text commands (0x0A cmd 4/5): msd.SHOW_TEXT.
const ShowText& city_show_text();

// ---------------------------------------------------------------- scripts (scripts.py, en_text.py)

/// en_text.graft_city_script: the JP AREAnn.PAK with the US script chunk, or nullopt.
std::optional<Bytes> graft_city_script(View jp_pak, View us_pak, std::string* why = nullptr);

struct ScriptSpec {
    const char* drive;   ///< "B" -> B.DRV
    const char* path;    ///< inside the DRV
    ShowText show_text;
    ButtonRegs button_regs;
};
const std::vector<ScriptSpec>& scenario_scripts();
/// scripts.graft: the US script made to run on the JP host, or nullopt (not the same program).
std::optional<Bytes> graft_script(View jp, View us, const ScriptSpec& spec, std::string* why = nullptr);

// ---------------------------------------------------------------- VCDIFF / xz

/// vcdiff.decode: `patch` applied to `source`. Throws std::runtime_error on a bad patch.
Bytes vcdiff_decode(View source, View patch);
/// A complete .xz stream (LZMA2 filter, any check), decoded; at most `limit` bytes.
Bytes xz_decode(View stream, size_t limit);

// ---------------------------------------------------------------- disc image, fixes

/// A form-1 file of the reference disc (manifest.json / layout.txt "file" lines).
struct DiscFile {
    std::string path;  ///< as on the disc ("B.DRV", "SLUS_013.28")
    uint32_t lba = 0, sectors = 0, size = 0;
};
/// The raw 2352-byte image an imported dump was made from (layout.txt + iso_meta.bin + fs/),
/// EDC/ECC recomputed, so it matches the original dump byte for byte; and its form-1 files.
Bytes rebuild_image(const std::filesystem::path& dump, std::vector<DiscFile>& files);

struct Fixed {
    std::map<std::string, Bytes> files;   ///< disc path -> fixed bytes
    std::vector<std::string> applied;     ///< patch names
    std::vector<std::string> problems;
};
/// fixes.apply: every patch applied to the reference image; the files they change.
Fixed apply_fixes(View image, const std::vector<DiscFile>& files,
                  const std::vector<std::pair<std::string, Bytes>>& patches);
/// fixes.port: the changes before -> after carried into `target` (another build of the file).
Bytes port_fix(View before, View after, View target, std::vector<std::string>* notes = nullptr);

// ---------------------------------------------------------------- catalog (catalog.py)

using Rows = std::vector<std::pair<std::string, Bytes>>;
/// A file of the catalog's game side: "EXE" or an overlay name, to its bytes.
using FileLookup = std::function<View(const std::string&)>;

Bytes jp_template(View raw);
Bytes us_template(View raw);
Bytes escape(View text);
/// catalog.read_own on one en*.tsv (text kept escaped), merged into `own`.
void read_own(View tsv, std::map<std::string, Bytes>& own);
/// catalog.build: (source rows, en rows); problems appended.
void build_catalog(std::string_view catalog_text, const FileLookup& jp, const FileLookup& us,
                   const std::map<std::string, Bytes>& own, Rows& source, Rows& en,
                   std::vector<std::string>& problems);
/// catalog.write_rows: "id\ttext\n" per row, the text escaped unless its id is in `escaped`.
Bytes write_rows(const Rows& rows, const std::map<std::string, Bytes>* escaped = nullptr);

// ---------------------------------------------------------------- big names (bigfont.py)

Bytes build_bigfont(View font_arc);
std::optional<Bytes> graft_match_name(View jp, View us);

// ---------------------------------------------------------------- cards (en_text.py)

/// en_font.bin from the US SYSTEM.TIM and boot executable.
Bytes build_font(View us_tim, View us_exe);
/// CARD2.CDD with the US names / attacks / effect text; overlong lines appended to `report`.
Bytes graft_cdd(View jp, View us, std::vector<std::string>& report);
struct LongName {
    Bytes prefix;
    int tag = 0;
    Bytes full;
};
/// DECK2.DEK with the US deck and owner names; the long deck names for en_names.txt.
Bytes graft_dek(View jp, View us, std::vector<std::string>& report, std::vector<LongName>& long_names);
/// Python's repr() of a bytes object (b'...'), for the report.
std::string py_repr(View b);

}  // namespace patch::text
