#pragma once
// Read-only access to the game disc: raw 2352-byte sectors from the data track of a .cue/.bin
// dump (the original image, so XA audio and STR video sectors keep their subheaders).

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace hle {

class Disc {
public:
    static constexpr uint32_t kRawSector = 2352;

    /// `image`: a .cue (its first data track is used) or a raw .bin. Throws on failure.
    explicit Disc(const std::filesystem::path& image);

    /// Raw sector at `lba` (sector 0 = MSF 00:02:00). Returns false past the end of the disc.
    bool read(uint32_t lba, uint8_t* out);
    uint32_t sector_count() const { return sectors_; }
    const std::filesystem::path& path() const { return bin_; }

    /// Read a file from the disc's root directory (ISO9660 name without ";1"); empty if absent.
    std::vector<uint8_t> read_root_file(const std::string& name);
    /// The boot executable named by SYSTEM.CNF (BOOT = cdrom:\\NAME;1), read from the disc.
    std::vector<uint8_t> read_boot_exe();

    /// Find a disc image: `hint` (a .cue/.bin given on the command line), DCB_DISC, the first
    /// .cue/.bin under disc/<serial>/, then the first .cue/.bin in the current directory.
    static std::filesystem::path locate(const std::string& serial, const std::filesystem::path& hint = {});

private:
    std::filesystem::path bin_;
    std::ifstream file_;
    uint32_t sectors_ = 0;
};

}  // namespace hle
