#pragma once
// Virtual File System: mount `.pak` archives and/or loose directories under one
// namespace, with later mounts shadowing earlier ones. This is the runtime side of
// the asset pipeline: `DCB_HD_PACK` points at either a `.pak` or a directory and
// the HD texture cache reads through this interface either way, so artists can
// iterate on loose files and ship a single archive.
//
// Deliberately dependency-free (stdlib + dcb_vfs only): PhysFS was rejected as a
// new native dependency with MinGW/static-link friction, and SDL_Storage is
// aimed at per-user save data rather than read-only asset mounts.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vfs {

class Vfs {
public:
    /// Mount a `.pak` archive (read-only). False when the file is not a valid pak.
    bool mount_pak(const std::filesystem::path& path);
    /// Mount a directory; `name` lookups resolve to `dir / name`. Later mounts win.
    void mount_dir(const std::filesystem::path& path);
    /// Convenience: pak file -> mount_pak, directory -> mount_dir, else false.
    bool mount(const std::filesystem::path& path);

    bool exists(const std::string& name) const;
    bool read(const std::string& name, std::vector<uint8_t>& out) const;
    size_t mount_count() const { return mounts_.size(); }

private:
    struct Mount {
        enum class Kind { Pak, Dir } kind;
        std::filesystem::path path;  ///< pak file, or directory root
    };
    std::vector<Mount> mounts_;
    mutable std::vector<std::string> errors_;  ///< sticky I/O diagnostics, for logs

    static bool valid_name(const std::string& name);
    static bool read_file(const std::filesystem::path& path, std::vector<uint8_t>& out);
};

}  // namespace vfs
