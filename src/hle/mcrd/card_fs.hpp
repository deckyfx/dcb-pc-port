#pragma once
// The PS1 kernel's memory-card file system ("bu00:" / "bu10:" devices), implemented on host
// buffers over MemoryCard images. The BIOS layer only translates guest pointers and registers:
//
//   B0:32 open(name, mode)          -> CardFs::open        v0 = fd or -1
//   B0:33 lseek(fd, offset, whence) -> CardFs::lseek       v0 = new position or -1
//   B0:34 read(fd, dst, len)        -> CardFs::read        v0 = bytes or -1
//   B0:35 write(fd, src, len)       -> CardFs::write       v0 = bytes or -1
//   B0:36 close(fd)                 -> CardFs::close       v0 = fd or -1
//   B0:41 format(dev)               -> CardFs::format      v0 = 1 / 0
//   B0:42 firstfile(pattern, dirent)-> CardFs::firstfile   v0 = dirent pointer / 0
//   B0:43 nextfile(dirent)          -> CardFs::nextfile    v0 = dirent pointer / 0
//   B0:44 rename(old, new)          -> CardFs::rename      v0 = 1 / 0
//   B0:45 erase(name)               -> CardFs::erase       v0 = 1 / 0
//   B0:54 GetLastError()            -> CardFs::last_error
//   B0:55 GetLastFileError(fd)      -> CardFs::file_error
//
// Everything completes synchronously. Files opened with FASYNC (0x8000) additionally expect a
// completion event after read/write (SwCARD F4000001h, which games wait on, plus HwCARD
// F0000011h for the sector transfer): last_was_async() / async_spec() say which.
// Directory frames are re-read from the card on every call, so raw sector writes through
// B0:4E stay coherent with this layer. Reference: psx-spx "Memory Card Data Format",
// "BIOS File Functions", "BIOS Memory Card Functions".

#include "mcrd/memcard.hpp"

#include <psx/state.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace hle {

class CardFs {
public:
    // open() mode bits (Psy-Q <sys/file.h>).
    static constexpr uint32_t kFRead = 0x0001;
    static constexpr uint32_t kFWrite = 0x0002;
    static constexpr uint32_t kFNonBlock = 0x0004;
    static constexpr uint32_t kFCreate = 0x0200;  ///< create; block count in bits 16-31 (0 = 1)
    static constexpr uint32_t kFAsync = 0x8000;

    // lseek() whence values the BIOS supports.
    static constexpr int kSeekSet = 0;
    static constexpr int kSeekCur = 1;

    // Kernel error codes (B0:54 GetLastError).
    static constexpr uint32_t kENoError = 0x00;
    static constexpr uint32_t kENoEnt = 0x02;   ///< file not found
    static constexpr uint32_t kEBadF = 0x09;    ///< bad fd / fd not opened for this access
    static constexpr uint32_t kEExist = 0x11;   ///< FCREAT / rename target already exists
    static constexpr uint32_t kENoDev = 0x13;   ///< no card in that slot / unknown device
    static constexpr uint32_t kEInval = 0x16;   ///< bad name, misaligned offset/length
    static constexpr uint32_t kEMFile = 0x18;   ///< no free file descriptor
    static constexpr uint32_t kENoSpc = 0x1C;   ///< not enough free blocks / write past the end

    // Card event specs for async completion (classes SwCARD F4000001h and HwCARD F0000011h).
    static constexpr uint32_t kSpecDone = 0x0004u;
    static constexpr uint32_t kSpecError = 0x8000u;

    static constexpr int kFirstFd = 2;   ///< fds 0/1 are the kernel TTY
    static constexpr int kMaxFds = 16;   ///< kernel FCB table size
    static constexpr uint32_t kBlockSize = 0x2000;
    static constexpr uint32_t kMaxName = 20;

    /// Guest DIRENTRY layout (40 bytes): memory-card attr = allocation state (0x51), size in
    /// bytes, next = 0 (the kernel leaves it for the caller), head = first block (1-15).
    struct DirEntry {
        char name[20];
        uint32_t attr, size, next, head;
        uint8_t system[4];
    };
    static_assert(sizeof(DirEntry) == 40);

    /// slot 0 = "bu00:", slot 1 = "bu10:"; nullptr = no card inserted.
    explicit CardFs(std::array<MemoryCard*, 2> slots);

    /// Swap the card in a slot (nullptr = remove); fds open on that slot are closed.
    void set_slot(int slot, MemoryCard* card);

    int open(const std::string& path, uint32_t mode);
    int read(int fd, uint8_t* dst, uint32_t bytes);
    int write(int fd, const uint8_t* src, uint32_t bytes);
    int lseek(int fd, int32_t offset, int whence);
    int close(int fd);

    /// Match `pattern` ("bu00:BASLPS-*", '?' = any char, '*' = rest of name) against the
    /// directory; on a hit fill `out` and remember the position for nextfile().
    bool firstfile(const std::string& pattern, DirEntry& out);
    bool nextfile(DirEntry& out);

    bool erase(const std::string& path);
    bool rename(const std::string& from, const std::string& to);
    bool format(const std::string& device);

    /// Number of unused (free or deleted) directory blocks on a slot, or -1 without a card.
    int free_blocks(int slot) const;

    uint32_t last_error() const { return last_error_; }
    /// Error of the last operation on `fd` (B0:55); kEBadF for an unknown fd.
    uint32_t file_error(int fd) const;
    /// True when the last read/write was on an FASYNC file: deliver the card completion events.
    bool last_was_async() const { return last_async_; }
    /// Spec to deliver with that event: kSpecDone on success, kSpecError on failure.
    uint32_t async_spec() const { return last_error_ == kENoError ? kSpecDone : kSpecError; }

    /// Save state: open files (positions, block chains), error codes and the firstfile/nextfile
    /// cursor (chunk "CDFS"). The cards themselves are not part of it: their images live on disk.
    void save_state(psx::StateWriter& w) const;
    void load_state(psx::StateReader& r);

private:
    static constexpr uint32_t kDirBlocks = 15;
    static constexpr uint32_t kFramesPerBlock = kBlockSize / MemoryCard::kFrameSize;
    static constexpr uint16_t kNoNext = 0xFFFF;

    struct File {
        bool used = false;
        int slot = 0;
        uint32_t head = 0;  ///< first block (1-15)
        uint32_t size = 0, pos = 0, mode = 0;
        uint32_t error = 0;
        std::array<uint8_t, kDirBlocks> chain{};  ///< block list, `chain_len` entries
        uint32_t chain_len = 0;
    };

    struct Path {
        int slot = -1;
        std::string name;
    };

    std::array<MemoryCard*, 2> slots_;
    std::array<File, kMaxFds> files_{};
    uint32_t last_error_ = 0;
    bool last_async_ = false;

    // firstfile/nextfile cursor.
    int find_slot_ = -1;
    std::string find_pattern_;
    uint32_t find_next_ = kDirBlocks + 1;

    bool fail(uint32_t error);
    bool parse(const std::string& path, Path& out, bool allow_empty_name = false);
    MemoryCard* card(int slot) const;
    File* file(int fd);

    /// Directory frame for block `b` (1-15) read from the card.
    static bool read_dir(const MemoryCard& c, uint32_t b, uint8_t* frame);
    static bool write_dir(MemoryCard& c, uint32_t b, uint8_t* frame);  ///< fixes the checksum
    static std::string dir_name(const uint8_t* frame);
    /// First block of the file named `name`, or 0.
    static uint32_t find(const MemoryCard& c, std::string_view name);
    /// Block chain from `head`; false when the links are broken.
    static bool chain(const MemoryCard& c, uint32_t head, File& f);
    static bool match(std::string_view pattern, std::string_view name);
    bool next_match(DirEntry& out);
    int transfer(int fd, uint8_t* dst, const uint8_t* src, uint32_t bytes);
};

}  // namespace hle
