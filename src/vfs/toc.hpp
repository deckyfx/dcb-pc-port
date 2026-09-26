#pragma once
// PSX DRV archive table-of-contents reader.
// Reference: tools/disc/pdrv_segments.py (P.DRV overlay table) and the format
// verified against SLPS-03101 + SLUS-01328: every *.DRV opens with 32-byte
// records — magic[4] + u32 sector + u32 size + u32 timestamp + name[16] — with
// payload at sector*2048. Data entries (\x01BIN, \x01PAK, \x01ARC, \x01TIM,
// \x01MSD, \x01CDD, \x01FNT) carry a size; \x80 group markers have size 0 and
// their sector points at a sub-TOC of the same shape (e.g. B.DRV CARD/FONT,
// A.DRV BGM). A zero record ends the table.
//
// No entry cap: E.DRV holds ~763 records, Z.DRV ~765. The walk is bounded by
// the blob size instead, and every record is sanity-checked (known kind,
// plausible name, sector/size in range) so garbage stops the parse.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vfs {

struct TocEntry {
    std::string magic;  // 4 raw bytes
    uint32_t sector = 0;
    uint32_t size = 0;
    std::string name;
    bool is_group = false;  ///< \x80 marker: sector points at a sub-TOC, size is 0
};

/// Parse the TOC at `data[base,...)`. Stops at the zero record, an unknown
/// record kind, an implausible name/sector/size, or the end of the blob.
/// Never reads past `size`.
std::vector<TocEntry> parse_toc(const uint8_t* data, size_t size, size_t base = 0);

}  // namespace vfs
