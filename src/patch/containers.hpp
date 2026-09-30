#pragma once
// Containers inside the DCB *.DRV archives, as the English-data builder reads and writes them.
// C++ port of tools/disc/drv_unpack.py (read_toc), tools/assets/dcb_containers.py (PAK) and the
// TIM / match-archive helpers of tools/text/{en_text,bigfont}.py; same checks, same results.
//
//   DRV  a tree of 32-byte TOC records (type, ext[3], u32 sector, u32 size, u32 stamp, name[16]),
//        root at sector 0; files start at sector * 0x800.
//   PAK  chunks `u16 kind, u16 id, u32 size, data` ended by u32 0xFFFFFFFF.
//   ARC  (B:\MATCH\NNN.ARC) u32 offsets, the last one the end of the file, then the TIMs.
//   TIM  u32 0x10, u32 flags (8: has a CLUT block), blocks `u32 len, u16 x, y, w, h, data`.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace patch {

using Bytes = std::vector<uint8_t>;
using View = std::span<const uint8_t>;

/// Little-endian reads; throw std::runtime_error past the end (Python's struct.error).
uint16_t rd16(View b, size_t off);
uint32_t rd32(View b, size_t off);
void wr16(Bytes& b, uint16_t v);
void wr32(Bytes& b, uint32_t v);

inline View view(std::string_view s) { return {reinterpret_cast<const uint8_t*>(s.data()), s.size()}; }
inline std::string_view str(View b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }
inline bool equal(View a, View b) { return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0); }

/// The whole file (throws with the path when it cannot be read).
Bytes read_file(const std::string& path);

// ---------------------------------------------------------------- DRV

/// A DRV archive: its file entries in TOC order (drv_unpack.read_toc). Names are the raw TOC
/// bytes (a few US names are Shift-JIS; the builder only looks up ASCII ones). Records that point
/// past the end of the archive (US B.DRV's stale "copy of CARD2.CDD") are left out.
class Drv {
public:
    struct Entry {
        std::string path;  ///< "DIR/NAME.EXT"
        uint32_t offset = 0, size = 0;
    };

    explicit Drv(View data);
    const std::vector<Entry>& entries() const { return entries_; }
    /// The first entry named `path` (TOC order); throws when there is none (Python's KeyError).
    View file(std::string_view path) const;
    /// The last entry named `path`, or nullopt (bigfont.drv_files: a dict, later entries win).
    std::optional<View> find_last(std::string_view path) const;
    View data() const { return data_; }

private:
    void walk(uint32_t sector, const std::string& prefix, int depth);

    View data_;
    std::vector<Entry> entries_;
};

// ---------------------------------------------------------------- PAK

struct PakChunk {
    uint16_t kind = 0, id = 0;
    Bytes data;
};
/// dcb_containers.read_pak: throws when a chunk overruns the file or non-zero bytes trail it.
std::vector<PakChunk> read_pak(View data);
/// dcb_containers.write_pak: the chunks, then the 0xFFFFFFFF terminator.
Bytes write_pak(const std::vector<PakChunk>& chunks);

// ---------------------------------------------------------------- TIM / ARC

struct TimBlock {
    size_t offset = 0;  ///< of the block's u32 length
    uint16_t x = 0, y = 0, w = 0, h = 0;
    View payload;
};
/// en_text.tim_blocks: the CLUT block (if any), then the image block.
std::vector<TimBlock> tim_blocks(View tim);

/// bigfont.arc_entries: the TIMs of a match archive, or nullopt when the offset table is not
/// one (the Python asserts, caught by graft_match_name).
std::optional<std::vector<View>> arc_entries(View arc);
/// bigfont.write_arc: offset table (with the end-of-file entry), then the entries.
Bytes write_arc(const std::vector<View>& entries);

}  // namespace patch
