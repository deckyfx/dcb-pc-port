#pragma once
// Named asset-load log for the RE loop (DCB_LOG_LOADS=1): one line per load
// event with the host frame number and guest cycle. Maps raw sector traffic
// to disc files and DRV archive entries so traces read as asset names, not
// LBAs. Guest-neutral: read-only lookups over data the disc layer already
// holds; logging goes to stderr, never to guest state or guest time.
//
//   frame 2710 load B.DRV:TITLE +0x0 41 KB
//   frame 2712 spu 0x1010 24 KB
//   frame 2712 xa file=1 channel=0 start
//   frame 2901 mdec start (DIGIMON.MOV seg 0)
//
// CD reads -> files: the ExtractedDisc range table (layout.txt) if the game
// runs from extracted data, else the ISO9660 directory. DRV entries: the
// file's own TOC via vfs::parse_toc (cached per file, first touch only).
// SPU: DMA channel 4 transfers into sound RAM (address + size). VAB matching
// by content hash is a follow-up (needs the wave-base answer); the log prints
// the transfer so the analyst can match it against sfx_manifest.json.
// MDEC: transfer counter edges (start = first output after idle). XA: file/
// channel filter edges from the CD controller.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace hle {

/// One parsed layout.txt range: LBA span -> file on disk.
struct LoadRange {
    uint32_t lba = 0, count = 0;
    std::string path;  // fs/... path for files, "" for meta/system ranges
    bool is_movie = false;
};

/// Sector -> file mapping, parsed once from the extracted layout (or empty
/// when running from a raw image: falls back to bare LBA ranges).
class LoadMap {
public:
    /// Parse extracted/<serial>/layout.txt. Returns false when absent.
    bool load_layout(const std::string& dir);
    /// Human-readable owner of `lba`: "B.DRV", "DIGIMON.MOV", "SYSTEM.CNF",
    /// "meta", or "lba NNNN".
    std::string owner(uint32_t lba) const;
    /// Full path on disk for a file range, or "" for meta/movie.
    std::string path_for(uint32_t lba) const;

private:
    std::vector<LoadRange> ranges_;
};

/// DRV archive entry cache: TOC per DRV file, parsed on first touch.
class DrvEntries {
public:
    /// Name the archive entry containing `offset` bytes into `file_bytes`
    /// (e.g. "TITLE" in B.DRV), or "" when unknown. `file_bytes` is the whole
    /// DRV file; callers pass what the disc layer already has mapped.
    std::string entry_at(const std::string& drv_path, const std::vector<uint8_t>& file_bytes, uint32_t offset);

private:
    struct Cached {
        std::string path;
        std::vector<std::pair<uint32_t, std::string>> spans;  // (start offset, name), sorted
    };
    std::vector<Cached> cache_;
};

/// The logger itself: enabled once from DCB_LOG_LOADS, fed by hook points in
/// CdRom (sector reads, XA edges), Mmio DMA (SPU, MDEC) and the host loop
/// (frame/cycle stamps). All methods are no-ops unless enabled.
class LoadLog {
public:
    static LoadLog& instance();

    bool enabled() const { return enabled_; }
    void set_enabled(bool on) { enabled_ = on; }

    /// Frame/cycle stamps, set by the host loop once per frame.
    void set_frame(uint64_t frame, uint64_t cycles) {
        frame_ = frame;
        cycles_ = cycles;
    }

    /// Point at the extracted data dir (for layout.txt). No-op when absent.
    void set_data_dir(const std::string& dir);

    /// A data (non-XA) sector was read. Coalesces contiguous ranges: prints on
    /// flush or when the stream breaks.
    void sector(uint32_t lba);
    /// XA audio edge from the CD controller.
    void xa(bool start, uint8_t file, uint8_t channel);
    /// SPU RAM upload via DMA channel 4 (address + byte size).
    void spu(uint32_t addr, uint32_t bytes);
    /// MDEC decode activity edge (true = output started after idle).
    void mdec(bool start);
    /// Once per host frame with the MDEC output counter: emits the stop edge
    /// when no output happened this frame while a decode was active.
    void mdec_frame(uint64_t transfers);
    /// A file opened through the native file layer (src/game/overrides/files.cpp): data that never
    /// goes through the CD drive. `loose` = served from assets/<serial>/files/.
    void file(const std::string& path, uint32_t bytes, bool loose);
    /// A streaming read (ReadS: movie video + XA audio) starts at `lba`; its sectors are not
    /// logged one by one.
    void stream(uint32_t lba);
    /// End of frame (or shutdown): flush any pending coalesced range.
    void flush();

private:
    LoadLog() = default;
    bool enabled_ = false;
    uint64_t frame_ = 0, cycles_ = 0;
    LoadMap map_;
    DrvEntries drvs_;

    // Coalesced sector run.
    bool have_run_ = false;
    uint32_t run_first_ = 0, run_last_ = 0;

    // Coalesced SPU run (contiguous sound-RAM uploads).
    bool have_spu_ = false;
    uint32_t spu_first_ = 0, spu_last_end_ = 0;

    // MDEC edge tracking.
    bool mdec_active_ = false;
    uint64_t mdec_seen_ = 0;

    void emit_run();
    void emit_spu();
};

}  // namespace hle
