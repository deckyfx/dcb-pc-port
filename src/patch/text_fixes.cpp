// Community fixes (.xdelta) for the game: port of tools/text/fixes.py.
//
// The fixes on romhacking.net target the US disc image, here only reference data: a fix corrects
// the files English is taken from, and a data change in an overlay (a table, not text) is ported
// into the SLPS overlay, found there by its unchanged surroundings. The patches are applied to
// the untouched image independently and merged (overlapping changes are refused).

#include "text_internal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <unordered_map>

namespace patch::text {

namespace {

constexpr size_t kRaw = 2352, kDataOffset = 24, kDataSize = 2048;

std::string hex(View b) {
    std::string s;
    char buf[3];
    for (uint8_t c : b) {
        std::snprintf(buf, sizeof buf, "%02x", c);
        s += buf;
    }
    return s;
}

/// Python's bytes.find(needle, from): the first index, or npos.
size_t find(View hay, View needle, size_t from = 0) {
    if (from > hay.size()) return std::string_view::npos;
    const auto it = std::search(hay.begin() + static_cast<std::ptrdiff_t>(from), hay.end(),
                                std::boyer_moore_horspool_searcher(needle.begin(), needle.end()));
    return it == hay.end() && !needle.empty() ? std::string_view::npos : static_cast<size_t>(it - hay.begin());
}

}  // namespace

Fixed apply_fixes(View image, const std::vector<DiscFile>& files,
                  const std::vector<std::pair<std::string, Bytes>>& patches) {
    Fixed result;
    if (patches.empty()) return result;
    std::unordered_map<size_t, uint8_t> merged;      // image byte -> fixed value
    std::unordered_map<size_t, std::string> owner;   // image byte -> the patch that set it
    for (const auto& [name, patch] : patches) {
        Bytes out;
        try {
            out = vcdiff_decode(image, patch);
        } catch (const std::exception& e) {
            result.problems.push_back(name + ": " + e.what());
            continue;
        }
        if (out.size() != image.size()) {
            result.problems.push_back(name + ": changes the image size (not a fix for this disc?)");
            continue;
        }
        // Only the user data of changed sectors counts (headers and EDC/ECC follow from it).
        // As in fixes.py, bytes merged before a clash stay merged.
        bool clash = false;
        for (size_t start = 0; start < out.size() && !clash; start += kRaw) {
            const size_t n = std::min(kRaw, out.size() - start);
            if (std::memcmp(out.data() + start, image.data() + start, n) == 0) continue;
            for (size_t i = start + kDataOffset; i < std::min(start + kDataOffset + kDataSize, out.size()); ++i) {
                if (out[i] == image[i]) continue;
                const auto own = owner.find(i);
                if (own != owner.end() && merged[i] != out[i]) {
                    char buf[48];
                    std::snprintf(buf, sizeof buf, " at image byte %#zx", i);
                    result.problems.push_back(name + ": overlaps " + own->second + buf);
                    clash = true;
                    break;
                }
                merged[i] = out[i];
                owner[i] = name;
            }
        }
        if (!clash) result.applied.push_back(name);
    }
    for (const DiscFile& f : files) {
        const bool changed = std::any_of(owner.begin(), owner.end(), [&](const auto& o) {
            const size_t lba = o.first / kRaw;
            return lba >= f.lba && lba < size_t{f.lba} + f.sectors;
        });
        if (!changed) continue;
        // fixes._file_bytes: the user data of the file's sectors, cut to its size.
        Bytes before;
        before.reserve(f.size);
        for (size_t k = 0; before.size() < f.size; ++k) {
            const size_t at = (f.lba + k) * kRaw + kDataOffset;
            if (at >= image.size()) break;
            const size_t n = std::min({kDataSize, size_t{f.size} - before.size(), image.size() - at});
            before.insert(before.end(), image.begin() + static_cast<std::ptrdiff_t>(at),
                          image.begin() + static_cast<std::ptrdiff_t>(at + n));
        }
        Bytes after = before;
        for (const auto& [i, v] : merged) {
            const size_t lba = i / kRaw, in_sector = i % kRaw;
            if (lba < f.lba || in_sector < kDataOffset || in_sector >= kDataOffset + kDataSize) continue;
            const size_t off = (lba - f.lba) * kDataSize + (in_sector - kDataOffset);
            if (off < after.size()) after[off] = v;
        }
        if (after != before) result.files[f.path] = std::move(after);
    }
    return result;
}

Bytes port_fix(View before, View after, View target, std::vector<std::string>* notes) {
    constexpr size_t kMinContext = 8, kMaxSide = 16;
    if (after.size() < before.size()) throw std::runtime_error("fix: the fixed file is shorter");
    Bytes out(target.begin(), target.end());
    for (size_t i = 0; i < before.size();) {
        if (before[i] == after[i]) {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < before.size() && before[i] != after[i]) ++i;
        const size_t end = i, n = end - start;
        // The smallest window of `before` around the run (at least kMinContext bytes, the run
        // included) that occurs exactly once in the target.
        size_t placed = std::string_view::npos;
        for (size_t total = std::max(kMinContext, n); total < n + 2 * kMaxSide + 1 && placed == std::string_view::npos;
             ++total) {
            for (size_t left = 0; left <= total - n; ++left) {
                const size_t right = total - n - left;
                if (left > kMaxSide || right > kMaxSide || left > start || end + right > before.size()) continue;
                const View window = before.subspan(start - left, total);
                const size_t hit = find(target, window);
                if (hit != std::string_view::npos && find(target, window, hit + 1) == std::string_view::npos) {
                    placed = hit + left;
                    break;
                }
            }
        }
        char buf[128];
        if (placed == std::string_view::npos) {
            std::snprintf(buf, sizeof buf, "%#zx: %zu byte(s) not found in the target build, left alone", start, n);
            if (notes) notes->push_back(buf);
            continue;
        }
        std::copy(after.begin() + static_cast<std::ptrdiff_t>(start), after.begin() + static_cast<std::ptrdiff_t>(end),
                  out.begin() + static_cast<std::ptrdiff_t>(placed));
        std::snprintf(buf, sizeof buf, "%#zx -> %#zx: ", start, placed);
        if (notes) notes->push_back(buf + hex(before.subspan(start, n)) + " -> " + hex(after.subspan(start, n)));
    }
    return out;
}

}  // namespace patch::text
