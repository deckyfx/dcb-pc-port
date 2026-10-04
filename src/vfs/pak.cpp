// Minimal `.pak` container. See pak.hpp for the layout.

#include "vfs/pak.hpp"

#include "vfs/hash.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace vfs {

FILE* open_file(const std::filesystem::path& path, const char* mode) {
#if defined(_WIN32)
    const std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfopen(path.c_str(), wmode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

bool seek_to(FILE* f, uint64_t offset) {
#if defined(_WIN32)
    return _fseeki64(f, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

namespace {

void put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
}

void put64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

bool get_bytes(FILE* f, uint8_t* out, size_t n) { return std::fread(out, 1, n, f) == n; }

bool get16(FILE* f, uint16_t& v) {
    uint8_t b[2];
    if (!get_bytes(f, b, 2)) return false;
    v = static_cast<uint16_t>(b[0] | (b[1] << 8));
    return true;
}

bool get64(FILE* f, uint64_t& v) {
    uint8_t b[8];
    if (!get_bytes(f, b, 8)) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b[i]) << (8 * i);
    return true;
}

constexpr size_t kChunk = 1 << 20;  ///< streaming copy granularity

/// Size and FNV-1a hash of a file, read in chunks.
bool hash_file(const std::filesystem::path& path, uint64_t& size, uint64_t& hash) {
    FILE* f = open_file(path, "rb");
    if (!f) return false;
    std::vector<uint8_t> chunk(kChunk);
    size = 0;
    hash = kFnvOffsetBasis;
    size_t n = 0;
    while ((n = std::fread(chunk.data(), 1, chunk.size(), f)) > 0) {
        hash = fnv1a64(chunk.data(), n, hash);
        size += n;
    }
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok;
}

bool valid_name_for_write(const std::string& name) {
    if (name.empty() || name.size() > 1023) return false;
    if (name.front() == '/' || name.back() == '/') return false;
    if (name.find('\\') != std::string::npos) return false;
    if (name.find("..") != std::string::npos) return false;
    for (char ch : name) {
        if (ch < 0x20 || ch == 0x7F) return false;
    }
    // Reject empty segments ("a//b").
    bool slash = false;
    for (char ch : name) {
        if (ch == '/') {
            if (slash) return false;
            slash = true;
        } else {
            slash = false;
        }
    }
    return true;
}

}  // namespace

bool PakWriter::check_name(const std::string& name) {
    if (!valid_name_for_write(name)) {
        error_ = "bad pak entry name: " + name;
        return false;
    }
    if (names_.count(name) != 0) {
        error_ = "duplicate pak entry: " + name;
        return false;
    }
    return true;
}

bool PakWriter::add(std::string name, const std::vector<uint8_t>& data) { return add(std::move(name), data.data(), data.size()); }

bool PakWriter::add(std::string name, const uint8_t* data, size_t size) {
    if (!check_name(name)) return false;
    names_.insert(name);
    Staged staged;
    staged.name = std::move(name);
    staged.data.assign(data, data + size);
    staged.size = size;
    staged.hash = fnv1a64(data, size);
    entries_.push_back(std::move(staged));
    return true;
}

bool PakWriter::add_file(std::string name, const std::filesystem::path& source) {
    if (!check_name(name)) return false;
    Staged staged;
    staged.source = source;
    if (!hash_file(source, staged.size, staged.hash)) {
        error_ = "cannot read " + source.string();
        return false;
    }
    names_.insert(name);
    staged.name = std::move(name);
    entries_.push_back(std::move(staged));
    return true;
}

uint64_t PakWriter::byte_size() const {
    uint64_t total = 12;
    for (const Staged& e : entries_) total += 2 + e.name.size() + 24 + e.size;
    return total;
}

uint64_t PakWriter::content_id() const {
    uint64_t id = kFnvOffsetBasis;
    for (const Staged& e : entries_) {
        id = fnv1a64(e.name.data(), e.name.size(), id);
        std::vector<uint8_t> numbers;
        put64(numbers, e.size);
        put64(numbers, e.hash);
        id = fnv1a64(numbers.data(), numbers.size(), id);
    }
    return id;
}

bool PakWriter::write(const std::string& path) const {
    FILE* f = open_file(path, "wb");
    if (!f) return false;
    bool ok = write(f);
    if (std::fclose(f) != 0) ok = false;
    return ok;
}

bool PakWriter::write(std::FILE* f) const {
    bool ok = true;

    // Header: magic + count.
    uint8_t header[12];
    std::memcpy(header, kPakMagic, 8);
    const uint32_t count = static_cast<uint32_t>(entries_.size());
    header[8] = static_cast<uint8_t>(count);
    header[9] = static_cast<uint8_t>(count >> 8);
    header[10] = static_cast<uint8_t>(count >> 16);
    header[11] = static_cast<uint8_t>(count >> 24);
    ok = ok && std::fwrite(header, 1, sizeof header, f) == sizeof header;

    // Table first so readers can memory-map-style seek; data offsets are from the archive start.
    size_t table_size = 0;
    for (const Staged& e : entries_) table_size += 2 + e.name.size() + 24;
    uint64_t data_at = 12 + table_size;
    std::vector<uint8_t> table;
    table.reserve(table_size);
    for (const Staged& e : entries_) {
        put16(table, static_cast<uint16_t>(e.name.size()));
        table.insert(table.end(), e.name.begin(), e.name.end());
        put64(table, data_at);
        put64(table, e.size);
        put64(table, e.hash);
        data_at += e.size;
    }
    ok = ok && std::fwrite(table.data(), 1, table.size(), f) == table.size();
    std::vector<uint8_t> chunk;
    for (const Staged& e : entries_) {
        if (!ok) break;
        if (e.source.empty()) {
            if (!e.data.empty()) ok = std::fwrite(e.data.data(), 1, e.data.size(), f) == e.data.size();
            continue;
        }
        // Streamed: the table already holds its size and hash, so the file must not have changed.
        FILE* in = open_file(e.source, "rb");
        if (!in) {
            error_ = "cannot read " + e.source.string();
            return false;
        }
        chunk.resize(kChunk);
        uint64_t done = 0, hash = kFnvOffsetBasis;
        size_t n = 0;
        while (ok && (n = std::fread(chunk.data(), 1, chunk.size(), in)) > 0) {
            hash = fnv1a64(chunk.data(), n, hash);
            done += n;
            ok = done <= e.size && std::fwrite(chunk.data(), 1, n, f) == n;
        }
        std::fclose(in);
        if (ok && (done != e.size || hash != e.hash)) {
            error_ = "file changed while packing: " + e.source.string();
            return false;
        }
    }
    return ok;
}

bool PakReader::open(const std::filesystem::path& file, uint64_t base, uint64_t length) {
    path_.clear();
    base_ = 0;
    entries_.clear();
    index_.clear();
    error_.clear();
    const std::string path = file.string();  // for messages
    FILE* f = open_file(file, "rb");
    if (!f) {
        error_ = "cannot open " + path;
        return false;
    }
    // file_size, not ftell: `long` is 32-bit on Windows and packs of upscaled art pass 2 GiB.
    // `total`: the archive's own size (the rest of the file, or the embedded span).
    std::error_code size_ec;
    const uint64_t file_total = std::filesystem::file_size(file, size_ec);
    const bool in_file = !size_ec && base <= file_total;
    const uint64_t rest = in_file ? file_total - base : 0;
    const uint64_t total = length != 0 ? std::min(length, rest) : rest;
    if (!in_file || total < 12 || !seek_to(f, base)) {
        error_ = "truncated header in " + path;
        std::fclose(f);
        return false;
    }
    uint8_t header[12];
    if (!get_bytes(f, header, sizeof header) || std::memcmp(header, kPakMagic, 8) != 0) {
        error_ = "bad magic in " + path;
        std::fclose(f);
        return false;
    }
    const uint32_t count =
        static_cast<uint32_t>(header[8] | (header[9] << 8) | (header[10] << 16) | (header[11] << 24));
    if (count > 1u << 20) {  // absurd: 1M entries would already be a ~30MB table
        error_ = "unreasonable file count in " + path;
        std::fclose(f);
        return false;
    }
    std::unordered_set<std::string> seen;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t name_len = 0;
        uint64_t offset = 0, size = 0, hash = 0;
        if (!get16(f, name_len) || name_len == 0 || name_len > 1023) {
            error_ = "bad name length in " + path;
            std::fclose(f);
            return false;
        }
        std::string name(name_len, '\0');
        if (!get_bytes(f, reinterpret_cast<uint8_t*>(name.data()), name_len) || !get64(f, offset) ||
            !get64(f, size) || !get64(f, hash)) {
            error_ = "truncated table in " + path;
            std::fclose(f);
            return false;
        }
        if (!valid_name_for_write(name) || !seen.insert(name).second) {
            error_ = "bad/duplicate name in " + path + ": " + name;
            std::fclose(f);
            return false;
        }
        // Offsets must be inside the archive and ranges must not wrap.
        if (offset > total || size > total ||
            offset + size < offset || offset + size > total) {
            error_ = "entry out of range in " + path + ": " + name;
            std::fclose(f);
            return false;
        }
        index_.emplace(name, entries_.size());
        entries_.push_back({std::move(name), offset, size, hash});
    }
    std::fclose(f);
    path_ = file;
    base_ = base;
    return true;
}

bool PakReader::find(const std::string& name, PakEntry& out) const {
    const auto it = index_.find(name);
    if (it == index_.end()) return false;
    out = entries_[it->second];
    return true;
}

bool PakReader::read(const std::string& name, std::vector<uint8_t>& out) const {
    PakEntry entry{"", 0, 0, 0};
    if (!find(name, entry)) return false;
    if (entry.size > (1ull << 32)) return false;
    std::ifstream in(path_, std::ios::binary);  // 64-bit seeks on every platform
    if (!in) return false;
    out.resize(static_cast<size_t>(entry.size));
    bool ok = true;
    if (entry.size > 0) {
        in.seekg(static_cast<std::streamoff>(base_ + entry.offset));
        ok = in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size())).good();
    }
    if (ok && fnv1a64(out.data(), out.size()) != entry.hash) ok = false;  // corrupt pak/data
    if (!ok) out.clear();
    return ok;
}

bool PakReader::extract(const PakEntry& entry, const std::filesystem::path& dest,
                        const std::function<void(uint64_t)>& on_bytes) const {
    FILE* in = open_file(path_, "rb");
    if (!in) return false;
    FILE* out = open_file(dest, "wb");
    if (!out) {
        std::fclose(in);
        return false;
    }
    bool ok = seek_to(in, base_ + entry.offset);
    std::vector<uint8_t> chunk(kChunk);
    uint64_t left = entry.size, hash = kFnvOffsetBasis;
    while (ok && left > 0) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(left, chunk.size()));
        ok = std::fread(chunk.data(), 1, n, in) == n && std::fwrite(chunk.data(), 1, n, out) == n;
        hash = fnv1a64(chunk.data(), n, hash);
        left -= n;
        if (ok && on_bytes) on_bytes(n);
    }
    std::fclose(in);
    if (std::fclose(out) != 0) ok = false;
    if (ok && hash != entry.hash) ok = false;  // corrupt archive
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(dest, ec);
    }
    return ok;
}

}  // namespace vfs
