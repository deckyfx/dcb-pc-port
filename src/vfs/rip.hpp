#pragma once
// The asset ripper's DRV walk and texture index (tools/asset_ripper.cpp `unpack`), as a library
// so the English-data builder (src/patch/art.cpp) sees exactly the manifest the ripper writes.
//
// rip_drv() walks a DRV's container table (vfs/toc.hpp, one level of \x80 groups), scans every
// payload for strictly-validated TIMs (vfs/tim.hpp) and adds one manifest entry per (TIM, palette
// row): content hashes (the runtime key, see src/hle/gpu/hd_textures.hpp) plus DRV provenance.
// Identical art (same RGBA after palette resolution) is named once: later copies point at the
// first copy's PNG path. Nothing is written here; the ripper passes sinks that write the payload
// .bins and encode the PNGs (stb_image_write stays tool-only, see vfs/image.hpp).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vfs {

struct Tim;
struct TocEntry;

struct RipEntry {
    uint64_t img = 0;
    uint64_t clut = 0;
    bool has_clut = false;
    int w = 0, h = 0, bpp = 0;
    std::string path;  ///< VFS-relative PNG path, '/'-separated (textures/<drv>/<name>.png)
    std::string drv;
    uint32_t drv_offset = 0;
    uint32_t drv_size = 0;
    uint64_t lba = 0;
    bool has_lba = false;
    std::string alt;            ///< the name this copy was found under (dedup provenance)
    std::vector<uint16_t> pal;  ///< indexed: the palette row this PNG was resolved with
};

class Ripper {
public:
    /// A DRV payload found by the walk (drv name, file stem, bytes). Return false when it could
    /// not be stored: its TIMs are then skipped. Unset: nothing is stored.
    std::function<bool(const std::string& drv, const std::string& stem, const uint8_t* data, size_t size)>
        on_payload;
    /// A new image (not a copy of one already named): its VFS path and RGBA8888 pixels. Return
    /// false when it could not be written: that palette variant then gets no entry. Unset: none.
    std::function<bool(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba)> on_image;
    /// Progress and warnings, one line each (no newline); `error` lines are the ripper's stderr.
    std::function<void(bool error, const std::string& line)> on_log;

    /// ISO LBA of each DRV file start (from extracted/<serial>/manifest.json), for provenance.
    std::vector<std::pair<std::string, uint64_t>> drv_lbas;

    /// Rip one DRV (`drv_name` is its file name, e.g. "B.DRV"). DRVs must come in a fixed order
    /// (the ripper sorts them) for the dedup names to be stable. False when `drv` is too short.
    bool rip_drv(const std::string& drv_name, const std::vector<uint8_t>& drv);

    const std::vector<RipEntry>& manifest() const { return manifest_; }
    /// assets_manifest.json for `game` (the format hd_textures.hpp reads).
    std::string manifest_json(const std::string& game) const;

    size_t payloads = 0, tims = 0, pngs = 0, png_reused = 0;

private:
    void log(bool error, const char* fmt, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 3, 4)))
#endif
        ;
    bool emit_tim(const Tim& tim, const std::string& drv, uint32_t payload_off, size_t tim_off,
                  const std::string& stem);
    void rip_payload(const std::vector<uint8_t>& drv, uint32_t off, uint32_t size, const std::string& drv_name,
                     const std::string& entry_name);
    void rip_toc_level(const std::vector<uint8_t>& drv, const std::vector<TocEntry>& toc,
                       const std::string& drv_name);

    std::vector<RipEntry> manifest_;
    /// FNV-1a of resolved RGBA pixels -> VFS path of the image already named.
    std::unordered_map<uint64_t, std::string> png_by_pixels_;
};

/// Characters outside [A-Za-z0-9_.-] become '_' (file names from TOC names); "" -> "unnamed".
std::string sanitize_name(std::string s);
/// Append `s` as a JSON string (quotes, `"`/`\` escaped, control characters as \u00XX).
void json_escape(std::string& out, const std::string& s);

}  // namespace vfs
