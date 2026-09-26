#pragma once
// Minimal `.pak` container: a clean bundle for processed/upscaled assets.
//
// Layout (all integers little-endian):
//   0x00  char[8]  magic "DCBPAK01"
//   0x08  u32      file count
//   then per file: u16 name length, name bytes (UTF-8, '/' separators, no leading slash),
//                  u64 data offset (absolute), u64 size, u64 FNV-1a hash of the data
//   then the concatenated file data.
// Names are limited to 1023 bytes; offsets/sizes to 4 GiB (checked on read).
// Rationale over zip/PhysFS: zero dependencies (the game binary stays SDL3+stdlib only),
// trivial to write correctly, and integrity-checked per file.

#include <cstdint>
#include <string>
#include <vector>

namespace vfs {

inline constexpr char kPakMagic[8] = {'D', 'C', 'B', 'P', 'A', 'K', '0', '1'};

struct PakEntry {
    std::string name;
    uint64_t offset = 0, size = 0, hash = 0;
};

class PakWriter {
public:
    /// Stage a file; names must be unique, relative, and use '/' separators.
    /// Returns false (and records an error) on duplicates or bad names.
    bool add(std::string name, const std::vector<uint8_t>& data);
    bool add(std::string name, const uint8_t* data, size_t size);
    /// Write the archive. Returns false on I/O error.
    bool write(const std::string& path) const;
    size_t file_count() const { return names_.size(); }
    const std::string& error() const { return error_; }

private:
    std::vector<std::string> names_;
    std::vector<std::vector<uint8_t>> blobs_;
    std::string error_;
};

class PakReader {
public:
    /// Open and validate an archive (magic, table bounds, per-file hash check is lazy).
    /// Returns false on any structural problem; `error()` says why.
    bool open(const std::string& path);
    const std::vector<PakEntry>& entries() const { return entries_; }
    bool find(const std::string& name, PakEntry& out) const;
    /// Read a whole file; verifies its FNV-1a hash. False when missing/corrupt/unreadable.
    bool read(const std::string& name, std::vector<uint8_t>& out) const;
    const std::string& error() const { return error_; }

private:
    std::string path_;
    std::vector<PakEntry> entries_;
    std::string error_;
};

}  // namespace vfs
