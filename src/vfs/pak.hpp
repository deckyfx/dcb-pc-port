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
// "Absolute" means from the start of the archive: an archive embedded in a larger file (the
// single-file build, vfs/payload.hpp) is the same bytes, opened with its base offset.
// Rationale over zip/PhysFS: zero dependencies (the game binary stays SDL3+stdlib only),
// trivial to write correctly, and integrity-checked per file.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vfs {

inline constexpr char kPakMagic[8] = {'D', 'C', 'B', 'P', 'A', 'K', '0', '1'};

/// fopen for a filesystem path: the wide API on Windows, so non-ASCII folders work.
std::FILE* open_file(const std::filesystem::path& path, const char* mode);
/// 64-bit absolute seek (a plain fseek takes a 32-bit long on Windows).
bool seek_to(std::FILE* f, uint64_t offset);

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
    /// Stage a file on disk without loading it: hashed now, streamed by write(). For bundles
    /// too big to hold in memory (the single-file payload). False if it cannot be read.
    bool add_file(std::string name, const std::filesystem::path& source);
    /// Write the archive. Returns false on I/O error.
    bool write(const std::string& path) const;
    /// Write the archive at `f`'s current position (offsets stay relative to its start).
    /// False on I/O error, or if a staged source file changed since add_file().
    bool write(std::FILE* f) const;
    size_t file_count() const { return entries_.size(); }
    /// Total bytes write() produces.
    uint64_t byte_size() const;
    /// Identity of the contents: a hash over every name, size and data hash, in order.
    uint64_t content_id() const;
    const std::string& error() const { return error_; }

private:
    struct Staged {
        std::string name;
        std::vector<uint8_t> data;       ///< in-memory data (add), or
        std::filesystem::path source;    ///< a file streamed at write time (add_file)
        uint64_t size = 0, hash = 0;
    };
    bool check_name(const std::string& name);
    std::vector<Staged> entries_;
    std::unordered_set<std::string> names_;
    mutable std::string error_;
};

class PakReader {
public:
    /// Open and validate an archive (magic, table bounds, per-file hash check is lazy).
    /// Returns false on any structural problem; `error()` says why. `base`/`length`: an
    /// archive embedded in a larger file, `length` bytes from `base` (0 = to the end).
    bool open(const std::filesystem::path& path, uint64_t base = 0, uint64_t length = 0);
    const std::vector<PakEntry>& entries() const { return entries_; }
    bool find(const std::string& name, PakEntry& out) const;
    /// Read a whole file; verifies its FNV-1a hash. False when missing/corrupt/unreadable.
    bool read(const std::string& name, std::vector<uint8_t>& out) const;
    /// Stream one entry into the file `dest` (replaced), verifying its hash, without holding it
    /// in memory. `on_bytes` is called with each chunk's size. False (and `dest` removed) on error.
    bool extract(const PakEntry& entry, const std::filesystem::path& dest,
                 const std::function<void(uint64_t)>& on_bytes = {}) const;
    const std::string& error() const { return error_; }

private:
    std::filesystem::path path_;
    uint64_t base_ = 0;
    std::vector<PakEntry> entries_;
    std::unordered_map<std::string, size_t> index_;  ///< name -> entries_ position
    std::string error_;
};

}  // namespace vfs
