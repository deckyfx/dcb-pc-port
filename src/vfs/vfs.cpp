// VFS: pak + directory mounts. See vfs.hpp for the rationale.

#include "vfs/vfs.hpp"

#include "vfs/pak.hpp"

#include <cstdio>
#include <cstring>

namespace vfs {

namespace {

std::string lower_ext(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return ext;
}

}  // namespace

bool Vfs::mount_pak(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
    auto reader = std::make_shared<PakReader>();
    if (!reader->open(path.string())) return false;
    mounts_.push_back({Mount::Kind::Pak, path, std::move(reader)});
    return true;
}

void Vfs::mount_dir(const std::filesystem::path& path) { mounts_.push_back({Mount::Kind::Dir, path, nullptr}); }

bool Vfs::mount(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) {
        if (lower_ext(path) == ".pak") return mount_pak(path);
        return false;
    }
    if (std::filesystem::is_directory(path, ec)) {
        mount_dir(path);
        return true;
    }
    return false;
}

bool Vfs::valid_name(const std::string& name) {
    if (name.empty() || name.size() > 1023 || name.front() == '/' || name.find('\\') != std::string::npos ||
        name.find("..") != std::string::npos) {
        return false;
    }
    for (char ch : name) {
        if (ch < 0x20 || ch == 0x7F) return false;
    }
    return true;
}

bool Vfs::read_file(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long total = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (total < 0 || total > (1l << 31)) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(total));
    const bool ok = out.empty() || std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    if (!ok) out.clear();
    return ok;
}

bool Vfs::exists(const std::string& name) const {
    if (!valid_name(name)) return false;
    std::vector<uint8_t> scratch;
    return read(name, scratch);
}

bool Vfs::read(const std::string& name, std::vector<uint8_t>& out) const {
    out.clear();
    if (!valid_name(name)) return false;
    // Later mounts shadow earlier ones.
    for (auto it = mounts_.rbegin(); it != mounts_.rend(); ++it) {
        if (it->kind == Mount::Kind::Dir) {
            const std::filesystem::path full = it->path / name;
            std::error_code ec;
            if (!std::filesystem::is_regular_file(full, ec)) continue;
            if (read_file(full, out)) return true;
            errors_.push_back("cannot read " + full.string());
        } else {
            PakEntry entry{"", 0, 0, 0};
            if (!it->pak->find(name, entry)) continue;
            if (it->pak->read(name, out)) return true;
            errors_.push_back("corrupt entry " + name + " in " + it->path.string());
        }
    }
    return false;
}

}  // namespace vfs
