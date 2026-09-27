#include "mcrd/card_fs.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace hle {

namespace {

constexpr uint32_t kFrame = MemoryCard::kFrameSize;

// Directory frame layout (psx-spx "Memory Card Data Format").
constexpr size_t kState = 0x00;     ///< allocation state
constexpr size_t kSize = 0x04;      ///< file size in bytes (first block only)
constexpr size_t kNext = 0x08;      ///< next block minus 1 (0-14), FFFFh = last
constexpr size_t kName = 0x0A;      ///< 20 chars + NUL
constexpr size_t kChecksum = 0x7F;  ///< XOR of bytes 00h-7Eh

constexpr uint8_t kFirst = 0x51, kMiddle = 0x52, kLast = 0x53;
constexpr uint8_t kFreeFresh = 0xA0;
constexpr uint8_t kDeletedOffset = kFreeFresh - 0x50;  ///< 51h/52h/53h -> A1h/A2h/A3h

bool is_free(uint8_t state) { return (state & 0xF0u) == 0xA0u; }

uint32_t get32(const uint8_t* p) {
    return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24;
}
void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
void put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

}  // namespace

CardFs::CardFs(std::array<MemoryCard*, 2> slots) : slots_(slots) {}

void CardFs::set_slot(int slot, MemoryCard* c) {
    if (slot < 0 || slot > 1) return;
    slots_[static_cast<size_t>(slot)] = c;
    for (File& f : files_) {
        if (f.used && f.slot == slot) f = File{};
    }
    if (find_slot_ == slot) find_next_ = kDirBlocks + 1;
}

bool CardFs::fail(uint32_t error) {
    last_error_ = error;
    return false;
}

MemoryCard* CardFs::card(int slot) const {
    return slot >= 0 && slot < 2 ? slots_[static_cast<size_t>(slot)] : nullptr;
}

CardFs::File* CardFs::file(int fd) {
    if (fd < kFirstFd || fd >= kMaxFds || !files_[static_cast<size_t>(fd)].used) return nullptr;
    return &files_[static_cast<size_t>(fd)];
}

uint32_t CardFs::file_error(int fd) const {
    if (fd < kFirstFd || fd >= kMaxFds || !files_[static_cast<size_t>(fd)].used) return kEBadF;
    return files_[static_cast<size_t>(fd)].error;
}

int CardFs::open_count() const {
    int n = 0;
    for (const File& f : files_)
        if (f.used) ++n;
    return n;
}

// "bu00:NAME" -> slot 0, "bu10:NAME" -> slot 1. The device name is case-insensitive; the file
// name is kept verbatim (card file names are case-sensitive).
bool CardFs::parse(const std::string& path, Path& out, bool allow_empty_name) {
    const size_t colon = path.find(':');
    if (colon != 4) return fail(kENoDev);
    std::string dev = path.substr(0, 4);
    std::transform(dev.begin(), dev.end(), dev.begin(), [](char ch) {
        return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    });
    if (dev == "bu00") {
        out.slot = 0;
    } else if (dev == "bu10") {
        out.slot = 1;
    } else {
        return fail(kENoDev);
    }
    out.name = path.substr(colon + 1);
    if (!card(out.slot)) return fail(kENoDev);
    if ((out.name.empty() && !allow_empty_name) || out.name.size() > kMaxName) return fail(kEInval);
    return true;
}

bool CardFs::read_dir(const MemoryCard& c, uint32_t b, uint8_t* frame) { return c.read_frame(b, frame); }

bool CardFs::write_dir(MemoryCard& c, uint32_t b, uint8_t* frame) {
    uint8_t x = 0;
    for (size_t i = 0; i < kChecksum; ++i) x ^= frame[i];
    frame[kChecksum] = x;
    return c.write_frame(b, frame);
}

std::string CardFs::dir_name(const uint8_t* frame) {
    const uint8_t* begin = frame + kName;
    const uint8_t* end = std::find(begin, begin + kMaxName, uint8_t{0});
    return std::string(begin, end);
}

uint32_t CardFs::find(const MemoryCard& c, std::string_view name) {
    uint8_t frame[kFrame];
    for (uint32_t b = 1; b <= kDirBlocks; ++b) {
        if (read_dir(c, b, frame) && frame[kState] == kFirst && dir_name(frame) == name) return b;
    }
    return 0;
}

bool CardFs::chain(const MemoryCard& c, uint32_t head, File& f) {
    uint8_t frame[kFrame];
    if (!read_dir(c, head, frame) || frame[kState] != kFirst) return false;
    f.head = head;
    f.size = get32(frame + kSize);
    f.chain_len = 0;
    uint32_t b = head;
    for (;;) {
        if (f.chain_len == kDirBlocks) return false;  // loop in the links
        f.chain[f.chain_len++] = static_cast<uint8_t>(b);
        const uint16_t next = get16(frame + kNext);
        if (next == kNoNext) break;
        if (next >= kDirBlocks) return false;
        b = next + 1u;
        if (!read_dir(c, b, frame) || (frame[kState] != kMiddle && frame[kState] != kLast)) return false;
    }
    f.size = std::min(f.size, f.chain_len * kBlockSize);
    return true;
}

int CardFs::free_blocks(int slot) const {
    const MemoryCard* c = card(slot);
    if (!c) return -1;
    uint8_t frame[kFrame];
    int n = 0;
    for (uint32_t b = 1; b <= kDirBlocks; ++b) {
        if (read_dir(*c, b, frame) && is_free(frame[kState])) ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------------------------
// open / close / read / write / lseek

int CardFs::open(const std::string& path, uint32_t mode) {
    last_async_ = false;
    Path p;
    if (!parse(path, p)) return -1;
    MemoryCard& c = *card(p.slot);

    int fd = -1;
    for (int i = kFirstFd; i < kMaxFds; ++i) {
        if (!files_[static_cast<size_t>(i)].used) {
            fd = i;
            break;
        }
    }
    if (fd < 0) return fail(kEMFile), -1;

    uint32_t head = find(c, p.name);
    if (mode & kFCreate) {
        if (head) return fail(kEExist), -1;
        const uint32_t blocks = std::max<uint32_t>(mode >> 16, 1);
        std::vector<uint32_t> picked;
        uint8_t frame[kFrame];
        for (uint32_t b = 1; b <= kDirBlocks && picked.size() < blocks; ++b) {
            if (read_dir(c, b, frame) && is_free(frame[kState])) picked.push_back(b);
        }
        if (picked.size() < blocks) return fail(kENoSpc), -1;
        for (size_t i = 0; i < picked.size(); ++i) {
            std::memset(frame, 0, sizeof frame);
            const bool last = i + 1 == picked.size();
            frame[kState] = i == 0 ? kFirst : last ? kLast : kMiddle;
            if (i == 0) {
                put32(frame + kSize, blocks * kBlockSize);
                std::memcpy(frame + kName, p.name.data(), p.name.size());
            }
            put16(frame + kNext, last ? kNoNext : static_cast<uint16_t>(picked[i + 1] - 1));
            write_dir(c, picked[i], frame);
        }
        head = picked[0];
        mode |= kFRead | kFWrite;
    } else if (!head) {
        return fail(kENoEnt), -1;
    }

    File f;
    if (!chain(c, head, f)) return fail(kENoEnt), -1;
    f.used = true;
    f.slot = p.slot;
    f.mode = (mode & (kFRead | kFWrite)) ? mode : mode | kFRead;  // mode 0 reads, like the kernel
    files_[static_cast<size_t>(fd)] = f;
    last_error_ = kENoError;
    return fd;
}

int CardFs::close(int fd) {
    last_async_ = false;
    File* f = file(fd);
    if (!f) return fail(kEBadF), -1;
    *f = File{};
    last_error_ = kENoError;
    return fd;
}

int CardFs::lseek(int fd, int32_t offset, int whence) {
    last_async_ = false;
    File* f = file(fd);
    if (!f) return fail(kEBadF), -1;
    int64_t target = offset;
    if (whence == kSeekCur) {
        target += f->pos;
    } else if (whence != kSeekSet) {
        f->error = kEInval;
        return fail(kEInval), -1;
    }
    if (target < 0 || target > f->size) {
        f->error = kEInval;
        return fail(kEInval), -1;
    }
    f->pos = static_cast<uint32_t>(target);
    f->error = last_error_ = kENoError;
    return static_cast<int>(f->pos);
}

int CardFs::read(int fd, uint8_t* dst, uint32_t bytes) { return transfer(fd, dst, nullptr, bytes); }

int CardFs::write(int fd, const uint8_t* src, uint32_t bytes) { return transfer(fd, nullptr, src, bytes); }

// Card files move whole 128-byte frames: position and length must be multiples of 128.
// Transfers are clipped at the end of the file; writing at the end fails with ENOSPC.
int CardFs::transfer(int fd, uint8_t* dst, const uint8_t* src, uint32_t bytes) {
    last_async_ = false;
    File* f = file(fd);
    if (!f) return fail(kEBadF), -1;
    last_async_ = (f->mode & kFAsync) != 0;
    const auto error = [&](uint32_t e) {
        f->error = e;
        fail(e);
        return -1;
    };
    if (!(f->mode & (dst ? kFRead : kFWrite))) return error(kEBadF);
    if (bytes % kFrame || f->pos % kFrame) return error(kEInval);
    MemoryCard* c = card(f->slot);
    if (!c) return error(kENoDev);

    const uint32_t n = std::min(bytes, f->size - f->pos);
    if (src && bytes && !n) return error(kENoSpc);
    uint32_t done = 0;
    while (done < n) {
        const uint32_t pos = f->pos + done;
        const uint32_t block = f->chain[pos / kBlockSize];
        const uint32_t first = block * kFramesPerBlock + (pos % kBlockSize) / kFrame;
        const uint32_t chunk = std::min(n - done, kBlockSize - pos % kBlockSize);  // stay in one block
        bool ok = true;
        if (src) {
            ok = c->write_frames(first, chunk / kFrame, src + done);
        } else {
            for (uint32_t i = 0; ok && i < chunk / kFrame; ++i) ok = c->read_frame(first + i, dst + done + i * kFrame);
        }
        if (!ok) return error(kEInval);
        done += chunk;
    }
    f->pos += n;
    f->error = last_error_ = kENoError;
    return static_cast<int>(n);
}

// ---------------------------------------------------------------------------------------------
// Directory

bool CardFs::match(std::string_view pattern, std::string_view name) {
    size_t i = 0;
    for (; i < pattern.size(); ++i) {
        if (pattern[i] == '*') return true;  // the kernel's '*' ends the comparison
        if (i >= name.size()) return false;
        if (pattern[i] != '?' && pattern[i] != name[i]) return false;
    }
    return i == name.size();
}

bool CardFs::next_match(DirEntry& out) {
    const MemoryCard* c = card(find_slot_);
    if (!c) return fail(kENoDev);
    uint8_t frame[kFrame];
    while (find_next_ <= kDirBlocks) {
        const uint32_t b = find_next_++;
        if (!read_dir(*c, b, frame) || frame[kState] != kFirst) continue;
        const std::string name = dir_name(frame);
        if (!match(find_pattern_, name)) continue;
        out = DirEntry{};
        std::memcpy(out.name, name.data(), name.size());
        out.attr = frame[kState];
        out.size = get32(frame + kSize);
        out.next = 0;
        out.head = b;
        last_error_ = kENoError;
        return true;
    }
    return fail(kENoEnt);
}

bool CardFs::firstfile(const std::string& pattern, DirEntry& out) {
    last_async_ = false;
    find_next_ = kDirBlocks + 1;
    Path p;
    if (!parse(pattern, p, true)) return false;
    find_slot_ = p.slot;
    find_pattern_ = p.name.empty() ? "*" : p.name;
    find_next_ = 1;
    return next_match(out);
}

bool CardFs::nextfile(DirEntry& out) {
    last_async_ = false;
    if (find_next_ > kDirBlocks) return fail(kENoEnt);
    return next_match(out);
}

bool CardFs::erase(const std::string& path) {
    last_async_ = false;
    Path p;
    if (!parse(path, p)) return false;
    MemoryCard& c = *card(p.slot);
    File f;
    const uint32_t head = find(c, p.name);
    if (!head || !chain(c, head, f)) return fail(kENoEnt);
    // Mark every block deleted (51h/52h/53h -> A1h/A2h/A3h); names and links stay, as the
    // kernel leaves them, so tools can still undelete.
    uint8_t frame[kFrame];
    for (uint32_t i = 0; i < f.chain_len; ++i) {
        if (!read_dir(c, f.chain[i], frame)) return fail(kEInval);
        frame[kState] = static_cast<uint8_t>(frame[kState] + kDeletedOffset);
        write_dir(c, f.chain[i], frame);
    }
    for (File& open_file : files_) {
        if (open_file.used && open_file.slot == p.slot && open_file.head == head) open_file = File{};
    }
    last_error_ = kENoError;
    return true;
}

bool CardFs::rename(const std::string& from, const std::string& to) {
    last_async_ = false;
    Path a, b;
    if (!parse(from, a) || !parse(to, b)) return false;
    if (a.slot != b.slot) return fail(kEInval);
    MemoryCard& c = *card(a.slot);
    const uint32_t head = find(c, a.name);
    if (!head) return fail(kENoEnt);
    if (find(c, b.name)) return fail(kEExist);
    uint8_t frame[kFrame];
    if (!read_dir(c, head, frame)) return fail(kEInval);
    std::memset(frame + kName, 0, kMaxName + 1);
    std::memcpy(frame + kName, b.name.data(), b.name.size());
    write_dir(c, head, frame);
    last_error_ = kENoError;
    return true;
}

bool CardFs::format(const std::string& device) {
    last_async_ = false;
    Path p;
    if (!parse(device, p, true)) return false;
    card(p.slot)->reformat();
    for (File& f : files_) {
        if (f.used && f.slot == p.slot) f = File{};
    }
    if (find_slot_ == p.slot) find_next_ = kDirBlocks + 1;
    last_error_ = kENoError;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Save state

void CardFs::save_state(psx::StateWriter& w) const {
    w.begin(psx::state_tag("CDFS"), 1);
    for (const File& f : files_) {
        w.boolean(f.used);
        w.pod(f.slot);
        w.u32(f.head);
        w.u32(f.size);
        w.u32(f.pos);
        w.u32(f.mode);
        w.u32(f.error);
        w.pod(f.chain);
        w.u32(f.chain_len);
    }
    w.u32(last_error_);
    w.boolean(last_async_);
    w.pod(find_slot_);
    w.str(find_pattern_);
    w.u32(find_next_);
    w.end();
}

void CardFs::load_state(psx::StateReader& r) {
    r.begin(psx::state_tag("CDFS"), 1);
    for (File& f : files_) {
        f.used = r.boolean();
        r.pod(f.slot);
        f.head = r.u32();
        f.size = r.u32();
        f.pos = r.u32();
        f.mode = r.u32();
        f.error = r.u32();
        r.pod(f.chain);
        f.chain_len = r.u32();
        if (f.slot < 0 || f.slot > 1 || f.chain_len > kDirBlocks) r.fail("bad memory-card file");
    }
    last_error_ = r.u32();
    last_async_ = r.boolean();
    r.pod(find_slot_);
    r.str(find_pattern_, 1024);
    find_next_ = r.u32();
    r.end();
}

}  // namespace hle
