// Single-file build: appending the bundle to the binary and unpacking it. See payload.hpp.

#include "vfs/payload.hpp"

#include "vfs/hash.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <vector>

namespace vfs {

namespace fs = std::filesystem;

namespace {

/// The stamp's first line while an unpack is under way (never a valid id).
constexpr const char* kUnpacking = "unpacking";

/// A pak entry name (UTF-8, '/' separators) as a path.
fs::path name_path(const std::string& name) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(name.data()), name.size()));
}

/// Names the reader accepts but a Windows path would not keep inside `dest` ("C:x").
bool safe_name(const std::string& name) { return name.find(':') == std::string::npos; }

std::string path_text(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

/// The stamp: its first line (an id in hex, or kUnpacking), then one file name per line.
bool read_stamp(const fs::path& path, std::string& head, std::vector<std::string>& files) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string line;
    if (!std::getline(in, head)) return false;
    while (std::getline(in, line))
        if (!line.empty()) files.push_back(line);
    return true;
}

bool write_stamp(const fs::path& path, const std::string& head, const std::set<std::string>& files) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << head << '\n';
    for (const std::string& f : files) out << f << '\n';
    out.close();
    return static_cast<bool>(out);
}

void put64(uint8_t* out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint64_t get64(const uint8_t* in) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(in[i]) << (8 * i);
    return v;
}

}  // namespace

bool write_payload_program(const fs::path& out, const fs::path& program, const PakWriter& pak, std::string& error) {
    std::error_code ec;
    const uint64_t program_size = fs::file_size(program, ec);
    if (ec) {
        error = "cannot read " + path_text(program);
        return false;
    }
    FILE* in = open_file(program, "rb");
    FILE* f = in ? open_file(out, "wb") : nullptr;
    if (!f) {
        error = in ? "cannot write " + path_text(out) : "cannot read " + path_text(program);
        if (in) std::fclose(in);
        return false;
    }
    // The program, unchanged.
    bool ok = true;
    std::vector<uint8_t> chunk(1 << 20);
    uint64_t copied = 0;
    size_t n = 0;
    while (ok && (n = std::fread(chunk.data(), 1, chunk.size(), in)) > 0) {
        ok = std::fwrite(chunk.data(), 1, n, f) == n;
        copied += n;
    }
    std::fclose(in);
    ok = ok && copied == program_size;
    // The archive, then the trailer.
    if (ok && !pak.write(f)) {
        error = pak.error().empty() ? "cannot write " + path_text(out) : pak.error();
        ok = false;
    }
    uint8_t trailer[kPayloadTrailerSize];
    put64(trailer, program_size);
    put64(trailer + 8, pak.byte_size());
    put64(trailer + 16, pak.content_id());
    std::memcpy(trailer + 24, kPayloadMagic, 8);
    ok = ok && std::fwrite(trailer, 1, sizeof trailer, f) == sizeof trailer;
    if (std::fclose(f) != 0) ok = false;
    if (!ok) {
        if (error.empty()) error = "cannot write " + path_text(out);
        fs::remove(out, ec);
    }
    return ok;
}

std::optional<PayloadTrailer> find_payload(const fs::path& program) {
    std::error_code ec;
    const uint64_t total = fs::file_size(program, ec);
    if (ec || total < kPayloadTrailerSize) return std::nullopt;
    std::ifstream in(program, std::ios::binary);
    uint8_t raw[kPayloadTrailerSize];
    in.seekg(static_cast<std::streamoff>(total - kPayloadTrailerSize));
    if (!in.read(reinterpret_cast<char*>(raw), sizeof raw)) return std::nullopt;
    if (std::memcmp(raw + 24, kPayloadMagic, 8) != 0) return std::nullopt;
    PayloadTrailer t;
    t.offset = get64(raw);
    t.size = get64(raw + 8);
    t.id = get64(raw + 16);
    // The archive sits right before the trailer.
    if (t.offset > total || t.size != total - kPayloadTrailerSize - t.offset) return std::nullopt;
    return t;
}

bool payload_keeps_existing(const std::string& name) { return name.rfind("cheats/", 0) == 0; }

PayloadStatus check_payload(const fs::path& program, const fs::path& dest, PayloadTrailer* trailer) {
    const std::optional<PayloadTrailer> found = find_payload(program);
    if (!found) return PayloadStatus::None;
    if (trailer) *trailer = *found;
    std::string head;
    std::vector<std::string> files;
    std::error_code ec;
    const bool stamped = read_stamp(dest / kPayloadStamp, head, files);
    if (stamped && head == to_hex16(found->id)) return PayloadStatus::UpToDate;
    // Someone else's assets/ (a zip install, a dev tree): not ours to replace.
    if (!stamped && fs::exists(dest / "assets", ec)) return PayloadStatus::NotOurs;
    return PayloadStatus::Needed;
}

PayloadResult unpack_payload(const fs::path& program, const fs::path& dest, const PayloadProgress& progress) {
    PayloadResult result;
    result.status = check_payload(program, dest, &result.trailer);
    if (result.status != PayloadStatus::Needed) return result;
    const std::string id = to_hex16(result.trailer.id);
    const fs::path stamp = dest / kPayloadStamp;
    std::string head;
    std::vector<std::string> old_files;
    std::error_code ec;
    read_stamp(stamp, head, old_files);  // a previous payload's files, if any
    const auto fail = [&](std::string why) {
        result.status = PayloadStatus::Failed;
        result.error = std::move(why);
        return result;
    };

    PakReader pak;
    if (!pak.open(program, result.trailer.offset, result.trailer.size)) return fail("the bundled data is damaged: " + pak.error());
    std::set<std::string> names;
    uint64_t total = 0;
    for (const PakEntry& e : pak.entries()) {
        if (!safe_name(e.name)) return fail("the bundled data is damaged: bad name " + e.name);
        names.insert(e.name);
        total += e.size;
    }

    // Claim the folder first: the stamp names every file of the old and the new payload, so an
    // interrupted unpack is redone (and cleaned up) on the next start. This is also where an
    // unwritable folder shows.
    fs::create_directories(stamp.parent_path(), ec);
    std::set<std::string> claimed(names);
    claimed.insert(old_files.begin(), old_files.end());
    if (!write_stamp(stamp, kUnpacking, claimed))
        return fail("cannot write to " + path_text(dest) + " (read-only folder?)");

    // The previous payload's files that this one no longer has.
    for (const std::string& name : old_files) {
        if (names.count(name) != 0 || payload_keeps_existing(name) || !safe_name(name) ||
            name.find("..") != std::string::npos || name.empty() || name.front() == '/')
            continue;
        if (fs::remove(dest / name_path(name), ec)) ++result.removed;
    }

    uint64_t done = 0;
    if (progress) progress(0, total);
    for (const PakEntry& e : pak.entries()) {
        const fs::path target = dest / name_path(e.name);
        if (payload_keeps_existing(e.name) && fs::exists(target, ec)) {
            ++result.kept;
            done += e.size;
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        const bool wrote = pak.extract(e, target, [&](uint64_t n) {
            done += n;
            if (progress) progress(done, total);
        });
        if (!wrote) return fail("cannot unpack " + path_text(target) + " (disk full, or damaged data?)");
        ++result.files;
        result.bytes += e.size;
    }
    if (!write_stamp(stamp, id, names)) return fail("cannot write " + path_text(stamp));
    if (progress) progress(total, total);
    result.status = PayloadStatus::Unpacked;
    return result;
}

}  // namespace vfs
