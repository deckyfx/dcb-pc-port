#include "cdrom/importer.hpp"

#include "cdrom/sha1.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

// The walk, the classification of files and the layout rules mirror tools/disc/extract_disc.py
// (walk_filesystem, classify, extract_file, write_layout) step for step: its output is the
// reference, and the two must stay byte-identical (see tests/import and
// tools/disc/verify_import.sh).

namespace hle::import {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kRawSector = 2352;
constexpr uint32_t kIsoSector = 2048;
constexpr uint32_t kChunk = 64;  ///< sectors per image read
constexpr uint8_t kSync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

// CD-XA directory-record attribute bits (big-endian word in the system use area).
constexpr uint16_t kXaForm2 = 0x1000;
constexpr uint16_t kXaInterleaved = 0x2000;
constexpr uint16_t kXaCdda = 0x4000;
constexpr uint8_t kSubmodeForm2 = 0x20;

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

uint32_t sectors_for(uint32_t size) { return static_cast<uint32_t>((uint64_t{size} + kIsoSector - 1) / kIsoSector); }

/// A path from UTF-8 text (file dialogs and ISO names), portable to Windows' wide paths.
fs::path utf8_path(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string mib(uint64_t bytes) { return std::to_string((bytes + (1u << 20) - 1) >> 20) + " MB"; }

// ---------------------------------------------------------------------------------------------
// The raw image

class Image {
public:
    explicit Image(const fs::path& path) : path_(path) {
        std::string ext = upper(path.extension().string());
        if (ext == ".CHD" || ext == ".PBP" || ext == ".ECM" || ext == ".CSO" || ext == ".ZIP" || ext == ".7Z")
            throw ImportError(ErrorCode::UnsupportedFormat,
                              path.filename().string() + ": " + ext.substr(1) +
                                  " images are not supported; convert it to .cue/.bin first (raw, 2352 bytes per sector)");
        file_.open(path, std::ios::binary);
        if (!file_) throw ImportError(ErrorCode::Io, "cannot open " + path.string());
        std::error_code ec;
        const uint64_t size = fs::file_size(path, ec);
        if (ec) throw ImportError(ErrorCode::Io, "cannot read the size of " + path.string() + ": " + ec.message());

        uint8_t head[16] = {};
        file_.read(reinterpret_cast<char*>(head), sizeof head);
        if (!file_ || std::memcmp(head, kSync, sizeof kSync) != 0) {
            // A cooked ISO (2048-byte sectors) has "CD001" at byte 0x8001.
            uint8_t cd001[6] = {};
            file_.clear();
            file_.seekg(16 * kIsoSector);
            file_.read(reinterpret_cast<char*>(cd001), sizeof cd001);
            if (file_ && std::memcmp(cd001 + 1, "CD001", 5) == 0)
                throw ImportError(ErrorCode::UnsupportedFormat,
                                  path.filename().string() +
                                      " is a 2048-byte ISO: it lacks the game's XA audio and movie sectors. "
                                      "Make a raw dump (.cue/.bin, 2352 bytes per sector) instead");
            if (size > 0 && size % kRawSector == 0)
                throw ImportError(ErrorCode::AudioTrack,
                                  path.filename().string() +
                                      " has no data sectors (an audio track?). Choose the .cue sheet, or the .bin of track 1");
            throw ImportError(ErrorCode::UnsupportedFormat, path.filename().string() + " is not a raw CD image (.cue/.bin)");
        }
        if (head[15] != 2)
            throw ImportError(ErrorCode::NotPlayStation,
                              path.filename().string() + " has a Mode " + std::to_string(head[15]) +
                                  " data track: not a PlayStation disc");
        if (size % kRawSector)
            std::fprintf(stderr, "[import] warning: image size %llu is not a multiple of %u\n",
                         static_cast<unsigned long long>(size), kRawSector);
        if (size / kRawSector > 0xFFFFFFFFull) throw ImportError(ErrorCode::UnsupportedFormat, "image too large for a CD");
        sectors_ = static_cast<uint32_t>(size / kRawSector);
        if (sectors_ < 18)
            throw ImportError(ErrorCode::Truncated, path.filename().string() + " is too short to hold a filesystem");
    }

    uint32_t sectors() const { return sectors_; }
    const fs::path& path() const { return path_; }

    /// `count` raw sectors from `lba` into `out` (count * 2352 bytes).
    void raw(uint32_t lba, uint32_t count, uint8_t* out) {
        if (uint64_t{lba} + count > sectors_)
            throw ImportError(ErrorCode::Truncated, "the image ends at sector " + std::to_string(sectors_) +
                                                        " but the disc needs sector " + std::to_string(uint64_t{lba} + count - 1) +
                                                        ": the dump is incomplete");
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(lba) * kRawSector);
        file_.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(count) * kRawSector);
        if (!file_) throw ImportError(ErrorCode::Io, "read error in " + path_.string());
    }

    /// The 2048-byte Form 1 user data of `count` sectors.
    std::vector<uint8_t> user(uint32_t lba, uint32_t count) {
        std::vector<uint8_t> out(static_cast<size_t>(count) * kIsoSector);
        std::vector<uint8_t> buf(static_cast<size_t>(kChunk) * kRawSector);
        for (uint32_t done = 0; done < count;) {
            const uint32_t n = std::min(kChunk, count - done);
            raw(lba + done, n, buf.data());
            for (uint32_t i = 0; i < n; ++i)
                std::memcpy(out.data() + static_cast<size_t>(done + i) * kIsoSector, buf.data() + static_cast<size_t>(i) * kRawSector + 24,
                            kIsoSector);
            done += n;
        }
        return out;
    }

private:
    fs::path path_;
    std::ifstream file_;
    uint32_t sectors_ = 0;
};

// ---------------------------------------------------------------------------------------------
// ISO9660 walk (extract_disc.py walk_filesystem)

enum class Storage { Form1, Raw2352, Cdda };

struct Entry {
    std::string path;  ///< "DIR/NAME" as on the disc (version and trailing dot removed)
    uint32_t lba = 0, size = 0, sectors = 0;
    bool has_xa = false;
    uint16_t xa = 0;
    Storage storage = Storage::Form1;
};

struct Scan {
    std::unique_ptr<Image> image;
    DiscInfo info;
    std::vector<Entry> files;                          ///< sorted by LBA (stable)
    std::vector<std::pair<uint32_t, uint32_t>> dirs;   ///< directory extents (lba, sectors)
};

/// extract_disc.py clean_name: ASCII (anything else becomes U+FFFD), no ";1", no trailing dots.
std::string clean_name(const uint8_t* p, size_t n) {
    std::string name;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] < 0x80)
            name += static_cast<char>(p[i]);
        else
            name += "\xEF\xBF\xBD";
    }
    name = name.substr(0, name.find(';'));
    while (!name.empty() && name.back() == '.') name.pop_back();
    if (name.empty() || name == "." || name == ".." || name.find_first_of(std::string("/\\\0", 3)) != std::string::npos)
        throw ImportError(ErrorCode::NotPlayStation, "the disc's filesystem has an unsafe file name");
    return name;
}

void walk(Scan& s) {
    Image& img = *s.image;
    const std::vector<uint8_t> pvd = img.user(16, 1);
    if (pvd[0] != 1 || std::memcmp(pvd.data() + 1, "CD001", 5) != 0)
        throw ImportError(ErrorCode::NotPlayStation, "no ISO9660 filesystem: not a PlayStation disc");
    if (le16(pvd.data() + 128) != kIsoSector)
        throw ImportError(ErrorCode::NotPlayStation, "unsupported ISO9660 block size: not a PlayStation disc");
    const uint32_t volume_sectors = le32(pvd.data() + 80);
    if (volume_sectors > img.sectors())
        throw ImportError(ErrorCode::Truncated, "the image has " + std::to_string(img.sectors()) +
                                                    " sectors but its filesystem spans " + std::to_string(volume_sectors) +
                                                    ": the dump is incomplete");
    s.info.volume_id = trim(std::string_view(reinterpret_cast<const char*>(pvd.data() + 40), 32));
    s.info.sectors = img.sectors();

    struct Dir {
        std::string path;
        uint32_t lba, size;
    };
    const uint8_t* root = pvd.data() + 156;
    std::vector<Dir> stack{{"", le32(root + 2), le32(root + 10)}};
    std::set<uint32_t> seen;
    while (!stack.empty()) {
        const Dir dir = stack.back();
        stack.pop_back();
        if (!seen.insert(dir.lba).second) continue;
        const uint32_t n = sectors_for(dir.size);
        s.dirs.emplace_back(dir.lba, n);
        const std::vector<uint8_t> data = img.user(dir.lba, n);
        for (size_t i = 0; i < data.size();) {
            const uint8_t length = data[i];
            if (length == 0) {  // records never straddle a sector: skip to the next one
                i = (i / kIsoSector + 1) * kIsoSector;
                continue;
            }
            if (length < 34 || i + length > data.size())
                throw ImportError(ErrorCode::NotPlayStation, "malformed directory record: not a PlayStation disc");
            const uint8_t* rec = data.data() + i;
            i += length;
            const uint8_t name_len = rec[32];
            if (33u + name_len > length)
                throw ImportError(ErrorCode::NotPlayStation, "malformed directory record: not a PlayStation disc");
            if (name_len == 1 && (rec[33] == 0 || rec[33] == 1)) continue;  // "." and ".."
            const std::string name = clean_name(rec + 33, name_len);
            const std::string path = dir.path.empty() ? name : dir.path + "/" + name;
            const uint32_t lba = le32(rec + 2), size = le32(rec + 10);
            if (rec[25] & 0x02) {
                stack.push_back({path, lba, size});
                continue;
            }
            Entry e{path, lba, size, sectors_for(size)};
            // XA attributes: system use area after the name, padded to an even offset.
            const size_t su = 33u + name_len + (1u - name_len % 2u);
            if (su + 14 <= length && rec[su + 6] == 'X' && rec[su + 7] == 'A') {
                e.has_xa = true;
                e.xa = static_cast<uint16_t>(rec[su + 4] << 8 | rec[su + 5]);
            }
            s.files.push_back(std::move(e));
        }
    }
    std::stable_sort(s.files.begin(), s.files.end(), [](const Entry& a, const Entry& b) { return a.lba < b.lba; });
}

/// Read a (Form 1) file of the scan into memory.
std::vector<uint8_t> read_file(Scan& s, const Entry& e) {
    std::vector<uint8_t> data = s.image->user(e.lba, e.sectors);
    data.resize(e.size);
    return data;
}

/// SYSTEM.CNF BOOT line -> boot file path, as extract_disc.py parse_system_cnf + boot_path.
std::string boot_line(const std::vector<uint8_t>& cnf) {
    std::string text(cnf.begin(), cnf.end());
    std::istringstream in(text);
    std::string line, boot;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos && upper(trim(line.substr(0, eq))) == "BOOT") boot = trim(line.substr(eq + 1));
    }
    return boot;
}

Scan scan(const fs::path& image) {
    Scan s;
    s.info.data_track = resolve_data_track(image);
    s.image = std::make_unique<Image>(s.info.data_track);
    walk(s);
    const auto cnf = std::find_if(s.files.begin(), s.files.end(), [](const Entry& e) { return upper(e.path) == "SYSTEM.CNF"; });
    if (cnf == s.files.end())
        throw ImportError(ErrorCode::NotPlayStation, "no SYSTEM.CNF on the disc: not a PlayStation game disc");
    const std::string boot = boot_line(read_file(s, *cnf));
    s.info.serial = serial_from_boot(boot);
    if (s.info.serial.empty())
        throw ImportError(ErrorCode::UnknownSerial,
                          "SYSTEM.CNF boots \"" + boot + "\", which carries no game serial: not a supported disc");
    return s;
}

/// Removes a temporary directory unless committed.
struct TempDir {
    fs::path path;
    bool committed = false;
    ~TempDir() {
        if (committed || path.empty()) return;
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void check_stream(const std::ofstream& out, const fs::path& path) {
    if (!out) throw ImportError(ErrorCode::Io, "write error on " + path.string() + " (disk full?)");
}

std::string hex4(const uint8_t* p) {
    char buf[9];
    std::snprintf(buf, sizeof buf, "%02x%02x%02x%02x", p[0], p[1], p[2], p[3]);
    return buf;
}

}  // namespace

// ---------------------------------------------------------------------------------------------

std::vector<CueTrack> parse_cue(std::string_view text, const fs::path& base_dir) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);
    std::vector<CueTrack> tracks;
    fs::path current;
    std::istringstream in{std::string(text)};
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream words(line);
        std::string keyword;
        if (!(words >> keyword)) continue;
        keyword = upper(keyword);
        if (keyword == "FILE") {
            std::string name;
            const size_t q1 = line.find('"');
            const size_t q2 = q1 == std::string::npos ? std::string::npos : line.find('"', q1 + 1);
            if (q2 != std::string::npos)
                name = line.substr(q1 + 1, q2 - q1 - 1);
            else
                words >> name;
            std::replace(name.begin(), name.end(), '\\', '/');
            current = base_dir / utf8_path(name);
        } else if (keyword == "TRACK") {
            CueTrack t;
            std::string number;
            words >> number >> t.type;
            t.number = std::atoi(number.c_str());
            t.type = upper(t.type);
            t.file = current;
            if (!t.type.empty()) tracks.push_back(std::move(t));
        }
    }
    return tracks;
}

fs::path resolve_data_track(const fs::path& image) {
    if (upper(image.extension().string()) != ".CUE") return image;
    std::ifstream in(image, std::ios::binary);
    if (!in) throw ImportError(ErrorCode::Io, "cannot open " + image.string());
    std::ostringstream text;
    text << in.rdbuf();
    const std::vector<CueTrack> tracks = parse_cue(text.str(), image.parent_path());
    if (tracks.empty()) throw ImportError(ErrorCode::UnsupportedFormat, image.filename().string() + " lists no tracks");
    const auto data = std::find_if(tracks.begin(), tracks.end(), [](const CueTrack& t) { return t.type.rfind("MODE", 0) == 0; });
    if (data == tracks.end())
        throw ImportError(ErrorCode::AudioTrack, image.filename().string() + " lists only audio tracks: not a game disc");
    if (data->type != "MODE2/2352")
        throw ImportError(data->type.rfind("MODE1", 0) == 0 ? ErrorCode::NotPlayStation : ErrorCode::UnsupportedFormat,
                          image.filename().string() + ": data track is " + data->type +
                              "; a PlayStation dump is MODE2/2352");
    std::error_code ec;
    if (fs::exists(data->file, ec)) return data->file;
    // Cue sheets written on Windows often disagree with the file's case.
    const std::string want = upper(data->file.filename().string());
    for (const auto& e : fs::directory_iterator(data->file.parent_path(), ec))
        if (upper(e.path().filename().string()) == want) return e.path();
    throw ImportError(ErrorCode::Io, image.filename().string() + " refers to " + data->file.filename().string() +
                                         ", which is missing (keep the .bin next to the .cue)");
}

std::string serial_from_boot(std::string_view boot) {
    std::string b = trim(boot);
    b = b.substr(0, b.find_first_of(" \t"));
    if (const size_t colon = b.find(':'); colon != std::string::npos) b = b.substr(colon + 1);
    b = b.substr(0, b.find(';'));
    std::replace(b.begin(), b.end(), '\\', '/');
    if (const size_t slash = b.rfind('/'); slash != std::string::npos) b = b.substr(slash + 1);
    std::string serial;
    for (char c : upper(b)) {
        if (c == '.') continue;
        serial += c == '_' ? '-' : c;
    }
    // AAAA-NNNNN
    if (serial.size() != 10 || serial[4] != '-') return {};
    for (size_t i = 0; i < 10; ++i) {
        if (i == 4) continue;
        const bool ok = i < 4 ? std::isupper(static_cast<unsigned char>(serial[i])) != 0
                              : std::isdigit(static_cast<unsigned char>(serial[i])) != 0;
        if (!ok) return {};
    }
    return serial;
}

const KnownGame* known_game(std::string_view serial) {
    for (const KnownGame& g : kKnownGames)
        if (serial == g.serial) return &g;
    return nullptr;
}

const char* known_title(std::string_view serial) {
    const KnownGame* g = known_game(serial);
    return g ? g->title : nullptr;
}

DiscInfo identify(const fs::path& image) { return scan(image).info; }

void verify_data_track(const fs::path& data_track, uint64_t size, std::string_view sha1, const std::string& name,
                       const ProgressFn& progress) {
    const std::string how =
        "\n\nThis program needs a clean dump matching redump.org (a raw .cue/.bin, 2352 bytes per sector, "
        "e.g. made with ImgBurn, cdrdao or redumper). You can check yours with any SHA-1 tool on the .bin.";
    std::error_code ec;
    const uint64_t actual_size = fs::file_size(data_track, ec);
    if (ec) throw ImportError(ErrorCode::Io, "cannot read " + data_track.string() + ": " + ec.message());
    // The size first: most bad dumps (wrong format, cut off, another pressing) fail here at once.
    if (actual_size != size)
        throw ImportError(ErrorCode::BadDump, name + ": " + data_track.filename().string() + " is " +
                                                  std::to_string(actual_size) + " bytes; the known-good dump is " +
                                                  std::to_string(size) + " bytes." + how);
    std::ifstream in(data_track, std::ios::binary);
    if (!in) throw ImportError(ErrorCode::Io, "cannot open " + data_track.string());
    Sha1 sha;
    Progress p;
    p.total = size;
    p.stage = "Verifying";
    p.item = data_track.filename().string();
    std::vector<char> buf(1u << 20);
    while (p.done < size) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), size - p.done));
        in.read(buf.data(), static_cast<std::streamsize>(want));
        if (static_cast<size_t>(in.gcount()) != want)
            throw ImportError(ErrorCode::Io, "read error on " + data_track.string());
        sha.update(buf.data(), want);
        p.done += want;
        if (progress && !progress(p)) throw ImportError(ErrorCode::Cancelled, "verification cancelled");
    }
    const std::string actual = Sha1::to_hex(sha.finish());
    if (actual != sha1)
        throw ImportError(ErrorCode::BadDump, name + ": " + data_track.filename().string() +
                                                  " does not match the known-good dump (SHA-1 " + actual +
                                                  ", expected " + std::string(sha1) +
                                                  "). It is damaged, modified (patched, translated) or "
                                                  "from another pressing." + how);
}

DiscInfo verify_dump(const fs::path& image, const ProgressFn& progress) {
    const DiscInfo info = identify(image);
    const KnownGame* game = known_game(info.serial);
    if (game == nullptr || game->data_sha1 == nullptr)
        throw ImportError(ErrorCode::UnknownSerial, "this is " + info.serial + " (\"" + info.volume_id +
                                                        "\"), not a supported disc");
    verify_data_track(info.data_track, game->data_size, game->data_sha1,
                      std::string(game->title) + " (" + game->serial + ", " + game->redump + ")", progress);
    return info;
}

Result import_disc(const fs::path& image, const fs::path& dest_root, const Options& options) {
    Scan s = scan(image);
    Image& img = *s.image;
    Result result;
    result.disc = s.info;
    const std::string& serial = s.info.serial;

    std::vector<std::string> accepted = options.accepted_serials;
    if (accepted.empty())
        for (const KnownGame& g : kKnownGames) accepted.emplace_back(g.serial);
    if (std::find(accepted.begin(), accepted.end(), serial) == accepted.end()) {
        std::string list;
        for (const std::string& a : accepted) list += (list.empty() ? "" : ", ") + a;
        throw ImportError(ErrorCode::UnknownSerial, "this is " + serial + " (\"" + s.info.volume_id +
                                                        "\"), not a supported disc (expected " + list + ")");
    }

    std::error_code ec;
    const fs::path final_dir = dest_root / serial;
    result.dir = final_dir;
    if (fs::exists(final_dir, ec) && !options.overwrite)
        throw ImportError(ErrorCode::AlreadyExists, final_dir.string() + " already exists");
    fs::create_directories(dest_root, ec);
    if (ec) throw ImportError(ErrorCode::Io, "cannot create " + dest_root.string() + ": " + ec.message());

    // Every non-CD-DA extent must lie inside the image (a cut-off dump fails here, before writing).
    for (const Entry& e : s.files) {
        if (e.has_xa && (e.xa & kXaCdda)) continue;
        if (uint64_t{e.lba} + e.sectors > img.sectors())
            throw ImportError(ErrorCode::Truncated, e.path + " lies past the end of the image (sector " +
                                                        std::to_string(uint64_t{e.lba} + e.sectors - 1) + " of " +
                                                        std::to_string(img.sectors()) + "): the dump is incomplete");
    }

    // Work units: sectors scanned for Form 2 + sectors copied + meta sectors.
    Progress progress;
    auto report = [&](uint64_t units, const char* stage, const std::string& item) {
        progress.done += units;
        progress.stage = stage;
        progress.item = item;
        if (options.progress && !options.progress(progress)) throw ImportError(ErrorCode::Cancelled, "import cancelled");
    };
    std::vector<uint8_t> covered(img.sectors(), 0);
    for (Entry& e : s.files) {
        const uint16_t attr = e.has_xa ? e.xa : 0;
        if (attr & kXaCdda) {
            e.storage = Storage::Cdda;
            continue;
        }
        if (!(attr & (kXaForm2 | kXaInterleaved))) progress.total += e.sectors;
        progress.total += e.sectors;
        std::fill_n(covered.begin() + e.lba, e.sectors, uint8_t{1});
    }
    for (const auto& [lba, n] : s.dirs) std::fill_n(covered.begin() + lba, n, uint8_t{0});
    std::vector<std::pair<uint32_t, uint32_t>> meta;  // (lba, count)
    for (uint32_t lba = 0; lba < img.sectors(); ++lba) {
        if (covered[lba]) continue;
        if (!meta.empty() && meta.back().first + meta.back().second == lba)
            ++meta.back().second;
        else
            meta.emplace_back(lba, 1);
    }
    uint64_t meta_sectors = 0;
    for (const auto& m : meta) meta_sectors += m.second;
    progress.total += meta_sectors;
    report(0, "Reading the disc", {});

    // Classify (extract_disc.py classify): XA-flagged or any Form 2 sector -> keep raw sectors.
    std::vector<uint8_t> buf(static_cast<size_t>(kChunk) * kRawSector);
    for (Entry& e : s.files) {
        if (e.storage == Storage::Cdda) continue;
        if (e.has_xa && (e.xa & (kXaForm2 | kXaInterleaved))) {
            e.storage = Storage::Raw2352;
            continue;
        }
        for (uint32_t done = 0; done < e.sectors && e.storage == Storage::Form1;) {
            const uint32_t n = std::min(kChunk, e.sectors - done);
            img.raw(e.lba + done, n, buf.data());
            for (uint32_t i = 0; i < n; ++i)
                if (buf[static_cast<size_t>(i) * kRawSector + 18] & kSubmodeForm2) e.storage = Storage::Raw2352;
            done += n;
        }
        report(e.sectors, "Reading the disc", e.path);
    }

    // Free space: exactly what will be written, plus some slack for the filesystem.
    uint64_t needed = meta_sectors * kRawSector + (1u << 20);
    for (const Entry& e : s.files) {
        if (e.storage == Storage::Raw2352) needed += uint64_t{e.sectors} * kRawSector;
        if (e.storage == Storage::Form1) needed += e.size;
    }
    if (const fs::space_info space = fs::space(dest_root, ec); !ec && space.available < needed)
        throw ImportError(ErrorCode::NoSpace, "not enough free space in " + dest_root.string() + ": the import needs " +
                                                  mib(needed) + ", " + mib(space.available) + " available");

    TempDir tmp{dest_root / ("." + serial + ".import-tmp")};
    fs::remove_all(tmp.path, ec);  // leftovers of an interrupted import
    fs::create_directories(tmp.path / "fs", ec);
    if (ec) throw ImportError(ErrorCode::Io, "cannot create " + tmp.path.string() + ": " + ec.message());

    // Files (extract_disc.py extract_file).
    for (const Entry& e : s.files) {
        if (e.storage == Storage::Cdda) continue;
        fs::path dest = tmp.path / "fs" / utf8_path(e.path);
        if (e.storage == Storage::Raw2352) dest += ".raw2352";
        fs::create_directories(dest.parent_path(), ec);
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) throw ImportError(ErrorCode::Io, "cannot create " + dest.string());
        uint32_t remaining = e.size;
        for (uint32_t done = 0; done < e.sectors;) {
            const uint32_t n = std::min(kChunk, e.sectors - done);
            img.raw(e.lba + done, n, buf.data());
            if (e.storage == Storage::Raw2352) {
                out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(n) * kRawSector);
                result.bytes += uint64_t{n} * kRawSector;
            } else {
                for (uint32_t i = 0; i < n; ++i) {
                    const uint32_t take = std::min(kIsoSector, remaining);
                    out.write(reinterpret_cast<const char*>(buf.data() + static_cast<size_t>(i) * kRawSector + 24), take);
                    remaining -= take;
                    result.bytes += take;
                }
            }
            check_stream(out, dest);
            done += n;
            report(n, "Copying files", e.path);
        }
        out.close();
        check_stream(out, dest);
        ++result.files;
    }

    // iso_meta.bin + layout.txt (extract_disc.py write_layout).
    std::string layout = "# dcb extracted disc layout v1\nsectors " + std::to_string(img.sectors()) + "\n";
    {
        const fs::path meta_path = tmp.path / "iso_meta.bin";
        std::ofstream out(meta_path, std::ios::binary | std::ios::trunc);
        uint64_t index = 0;
        for (const auto& [lba, count] : meta) {
            for (uint32_t done = 0; done < count;) {
                const uint32_t n = std::min(kChunk, count - done);
                img.raw(lba + done, n, buf.data());
                out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(n) * kRawSector);
                check_stream(out, meta_path);
                done += n;
                report(n, "Finishing", "iso_meta.bin");
            }
            layout += "meta " + std::to_string(lba) + " " + std::to_string(count) + " " + std::to_string(index) + "\n";
            index += count;
        }
        out.close();
        check_stream(out, meta_path);
        result.bytes += index * kRawSector;
    }
    uint8_t first[kRawSector], last[kRawSector];
    for (const Entry& e : s.files) {
        if (e.storage == Storage::Cdda || e.sectors == 0) continue;
        img.raw(e.lba, 1, first);
        img.raw(e.lba + e.sectors - 1, 1, last);
        const bool raw = e.storage == Storage::Raw2352;
        layout += "file " + std::to_string(e.lba) + " " + std::to_string(e.sectors) + " " + std::to_string(e.size) +
                  (raw ? " raw2352 " : " form1 ") + hex4(first + 16) + " " + hex4(last + 16) + " fs/" + e.path +
                  (raw ? ".raw2352" : "") + "\n";
    }
    {
        const fs::path layout_path = tmp.path / "layout.txt";
        std::ofstream out(layout_path, std::ios::binary | std::ios::trunc);
        out.write(layout.data(), static_cast<std::streamsize>(layout.size()));
        out.close();
        check_stream(out, layout_path);
    }
    if (options.progress && !options.progress(progress)) throw ImportError(ErrorCode::Cancelled, "import cancelled");

    // Commit: swap the finished tree into place.
    const fs::path old = dest_root / ("." + serial + ".import-old");
    fs::remove_all(old, ec);
    if (fs::exists(final_dir, ec)) {
        fs::rename(final_dir, old, ec);
        if (ec) throw ImportError(ErrorCode::Io, "cannot replace " + final_dir.string() + ": " + ec.message());
    }
    fs::rename(tmp.path, final_dir, ec);
    if (ec) throw ImportError(ErrorCode::Io, "cannot move the import into " + final_dir.string() + ": " + ec.message());
    tmp.committed = true;
    fs::remove_all(old, ec);
    return result;
}

}  // namespace hle::import
