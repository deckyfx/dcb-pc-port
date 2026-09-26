// Minimal `.pak` container. See pak.hpp for the layout.

#include "vfs/pak.hpp"

#include "vfs/hash.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace vfs {

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

bool PakWriter::add(std::string name, const std::vector<uint8_t>& data) { return add(std::move(name), data.data(), data.size()); }

bool PakWriter::add(std::string name, const uint8_t* data, size_t size) {
    if (!valid_name_for_write(name)) {
        error_ = "bad pak entry name: " + name;
        return false;
    }
    for (const auto& n : names_) {
        if (n == name) {
            error_ = "duplicate pak entry: " + name;
            return false;
        }
    }
    names_.push_back(std::move(name));
    blobs_.emplace_back(data, data + size);
    return true;
}

bool PakWriter::write(const std::string& path) const {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = true;

    // Header: magic + count placeholder (patched after the table is fixed up).
    uint8_t header[12];
    std::memcpy(header, kPakMagic, 8);
    const uint32_t count = static_cast<uint32_t>(names_.size());
    header[8] = static_cast<uint8_t>(count);
    header[9] = static_cast<uint8_t>(count >> 8);
    header[10] = static_cast<uint8_t>(count >> 16);
    header[11] = static_cast<uint8_t>(count >> 24);
    ok = ok && std::fwrite(header, 1, sizeof header, f) == sizeof header;

    // Table first so readers can memory-map-style seek; data offsets are absolute.
    size_t table_size = 0;
    for (size_t i = 0; i < names_.size(); ++i) table_size += 2 + names_[i].size() + 24;
    uint64_t data_at = 12 + table_size;
    std::vector<uint8_t> table;
    table.reserve(table_size);
    for (size_t i = 0; i < names_.size(); ++i) {
        put16(table, static_cast<uint16_t>(names_[i].size()));
        table.insert(table.end(), names_[i].begin(), names_[i].end());
        put64(table, data_at);
        put64(table, blobs_[i].size());
        put64(table, fnv1a64(blobs_[i].data(), blobs_[i].size()));
        data_at += blobs_[i].size();
    }
    ok = ok && std::fwrite(table.data(), 1, table.size(), f) == table.size();
    for (const auto& blob : blobs_) {
        if (!blob.empty()) ok = ok && std::fwrite(blob.data(), 1, blob.size(), f) == blob.size();
    }
    if (std::fclose(f) != 0) ok = false;
    return ok;
}

bool PakReader::open(const std::string& path) {
    path_.clear();
    entries_.clear();
    index_.clear();
    error_.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error_ = "cannot open " + path;
        return false;
    }
    // file_size, not ftell: `long` is 32-bit on Windows and packs of upscaled art pass 2 GiB.
    std::error_code size_ec;
    const uint64_t total = std::filesystem::file_size(path, size_ec);
    if (size_ec || total < 12) {
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
        // Offsets must be inside the file and ranges must not wrap.
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
    path_ = path;
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
        in.seekg(static_cast<std::streamoff>(entry.offset));
        ok = in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size())).good();
    }
    if (ok && fnv1a64(out.data(), out.size()) != entry.hash) ok = false;  // corrupt pak/data
    if (!ok) out.clear();
    return ok;
}

}  // namespace vfs
