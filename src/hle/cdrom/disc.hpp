#pragma once
// Where the game's data comes from. The CD-ROM model asks for raw 2352-byte sectors by LBA; two
// sources can answer:
//   ImageDisc      the original .cue/.bin dump
//   ExtractedDisc  files imported from the dump by `dcb --import` / the first-run import
//                  (importer.hpp) or tools/disc/extract_disc.py (layout.txt + fs/ + iso_meta.bin):
//                  sectors are rebuilt on demand, so the game runs without the disc image and
//                  individual files can later be replaced (translation, enhanced assets).

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hle {

class Disc {
public:
    static constexpr uint32_t kRawSector = 2352;

    virtual ~Disc() = default;

    /// Raw sector at `lba` (sector 0 = MSF 00:02:00). Returns false past the end of the disc.
    virtual bool read(uint32_t lba, uint8_t* out) = 0;
    virtual uint32_t sector_count() const = 0;
    /// Human-readable origin, for logs.
    virtual std::string describe() const = 0;

    /// Read a file from the disc's root directory (ISO9660 name without ";1"); empty if absent.
    std::vector<uint8_t> read_root_file(const std::string& name);
    /// The boot executable named by SYSTEM.CNF (BOOT = cdrom:\\NAME;1), read from the disc.
    std::vector<uint8_t> read_boot_exe();

    /// Open `path`: a directory with layout.txt (extracted data), a .cue, or a raw .bin.
    static std::unique_ptr<Disc> open(const std::filesystem::path& path);

    /// Find the game data: `hint` (command-line argument), DCB_DISC, extracted/<serial>/ (native
    /// data, preferred), then a .cue/.bin under disc/<serial>/ or in the current directory.
    /// Throws with instructions (import the dump) when nothing is found.
    static std::filesystem::path locate(const std::string& serial, const std::filesystem::path& hint = {});
    /// As locate(), but returns an empty path when nothing is found.
    static std::filesystem::path find(const std::string& serial, const std::filesystem::path& hint = {});
};

/// The original dump: first data track of a .cue, or a raw .bin.
class ImageDisc final : public Disc {
public:
    explicit ImageDisc(const std::filesystem::path& image);
    bool read(uint32_t lba, uint8_t* out) override;
    uint32_t sector_count() const override { return sectors_; }
    std::string describe() const override { return "disc image " + bin_.string(); }

private:
    std::filesystem::path bin_;
    std::ifstream file_;
    uint32_t sectors_ = 0;
};

/// Sectors rebuilt from extracted files (see importer.hpp / tools/disc/extract_disc.py write_layout()).
///
/// File overrides: a file at `assets/<serial>/disc/<name>` (the name as under fs/; the older
/// `<dir>/overrides/<name>` also works) replaces that disc file, provided it is exactly the same
/// size; anything else is refused and logged. Raw 2352-byte
/// files (movies) get each sector header re-stamped with the position it is served at, so a file
/// taken from another pressing (e.g. the US movie) reads as if it were on this disc.
class ExtractedDisc final : public Disc {
public:
    explicit ExtractedDisc(const std::filesystem::path& dir);
    bool read(uint32_t lba, uint8_t* out) override;
    uint32_t sector_count() const override { return sectors_; }
    std::string describe() const override { return "extracted data " + dir_.string(); }

private:
    struct Range {
        enum Kind : uint8_t { Meta, Form1, Raw } kind = Meta;
        uint32_t lba = 0, count = 0;
        uint32_t meta_index = 0;       ///< Meta: first sector's index in iso_meta.bin
        uint32_t bytes = 0;            ///< Form1: file size
        uint8_t first_sh[4] = {}, last_sh[4] = {};  ///< Form1: subheaders (last one marks EOF)
        std::filesystem::path path;
        bool overridden = false;       ///< served from overrides/ (Raw: headers re-stamped)
    };

    std::filesystem::path dir_;
    uint32_t sectors_ = 0;
    std::map<uint32_t, Range> ranges_;  ///< keyed by first LBA
    std::map<std::string, std::ifstream> open_;

    std::ifstream& stream(const std::filesystem::path& path);
    /// Point `r` at overrides/<name> when a same-size replacement exists (see the class comment).
    void apply_override(Range& r, const std::string& rel);
};

}  // namespace hle
