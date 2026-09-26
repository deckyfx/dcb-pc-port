#pragma once
// A memory card backed by a raw 128 KB image file (the ".mcd" format emulators use), so saves
// move freely between this port and emulators. 1024 frames of 128 bytes; 16 blocks of 8 KB.
// Reference: psx-spx "Memory Card Data Format".

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace hle {

class MemoryCard {
public:
    static constexpr uint32_t kFrameSize = 128;
    static constexpr uint32_t kFrames = 1024;
    static constexpr uint32_t kSize = kFrameSize * kFrames;

    /// Open `path`; a missing file becomes a freshly formatted card (written on first save).
    explicit MemoryCard(std::filesystem::path path);

    bool read_frame(uint32_t frame, uint8_t* out) const;
    bool write_frame(uint32_t frame, const uint8_t* in);  ///< persists immediately
    /// Write `count` consecutive frames from `in` (count * 128 bytes) and persist once.
    bool write_frames(uint32_t first, uint32_t count, const uint8_t* in);
    /// Erase the card to a freshly formatted state (header, empty directory) and persist.
    void reformat();

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    std::vector<uint8_t> data_;

    void format();
    void save() const;
};

}  // namespace hle
