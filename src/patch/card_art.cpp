#include "patch/card_art.hpp"

#include "vfs/image.hpp"
#include "vfs/tim.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <numeric>

namespace patch::card_art {

namespace {

constexpr uint32_t kTimMagic = 0x10, kTim8 = 9;  // 8-bit with a palette

uint16_t to15(int r, int g, int b) {
    const auto c = [](int v) { return std::clamp((v * 31 + 127) / 255, 0, 31); };
    const auto v = static_cast<uint16_t>(c(r) | c(g) << 5 | c(b) << 10);
    return v == 0 ? 0x8000 : v;  // 0x0000 is transparent: opaque black has the STP bit
}

void put16(Bytes& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}

void put32(Bytes& b, uint32_t v) {
    put16(b, static_cast<uint16_t>(v));
    put16(b, static_cast<uint16_t>(v >> 16));
}

const uint8_t* px(const Image& img, int x, int y) {
    return &img.rgba[(static_cast<size_t>(y) * static_cast<size_t>(img.w) + static_cast<size_t>(x)) * 4];
}

/// The dark backdrop: a vertical gradient, dimmer at the sides.
std::array<int, 3> backdrop(int x, int y, int size) {
    const double t = static_cast<double>(y) / std::max(1, size - 1);
    const double dx = (static_cast<double>(x) - (size - 1) / 2.0) / (size / 2.0);
    const double v = 1.0 - 0.45 * dx * dx;
    return {static_cast<int>((52 - 40 * t) * v), static_cast<int>((40 - 32 * t) * v), static_cast<int>((70 - 54 * t) * v)};
}

/// Median cut: up to `n` colours for `colours` (r, g, b triples).
std::vector<std::array<int, 3>> median_cut(std::vector<std::array<int, 3>> colours, size_t n) {
    struct Box {
        size_t begin, end;
    };
    std::vector<Box> boxes{{0, colours.size()}};
    const auto widest = [&](const Box& b, int& channel) {
        int best = -1;
        for (int c = 0; c < 3; ++c) {
            int lo = 255, hi = 0;
            for (size_t i = b.begin; i < b.end; ++i) {
                lo = std::min(lo, colours[i][static_cast<size_t>(c)]);
                hi = std::max(hi, colours[i][static_cast<size_t>(c)]);
            }
            if (hi - lo > best) {
                best = hi - lo;
                channel = c;
            }
        }
        return best;
    };
    while (boxes.size() < n) {
        size_t pick = boxes.size();
        int pick_channel = 0;
        long pick_score = 0;
        for (size_t i = 0; i < boxes.size(); ++i) {
            if (boxes[i].end - boxes[i].begin < 2) continue;
            int channel = 0;
            const long score = static_cast<long>(widest(boxes[i], channel)) * static_cast<long>(boxes[i].end - boxes[i].begin);
            if (score > pick_score) {
                pick_score = score;
                pick = i;
                pick_channel = channel;
            }
        }
        if (pick == boxes.size()) break;  // nothing left to split
        Box& b = boxes[pick];
        const auto first = colours.begin() + static_cast<long>(b.begin), last = colours.begin() + static_cast<long>(b.end);
        const auto mid = first + (last - first) / 2;
        std::nth_element(first, mid, last, [&](const auto& l, const auto& r) {
            return l[static_cast<size_t>(pick_channel)] < r[static_cast<size_t>(pick_channel)];
        });
        const size_t split = static_cast<size_t>(mid - colours.begin());
        boxes.push_back({split, b.end});
        b.end = split;
    }
    std::vector<std::array<int, 3>> out;
    for (const Box& b : boxes) {
        std::array<long, 3> sum{};
        for (size_t i = b.begin; i < b.end; ++i)
            for (size_t c = 0; c < 3; ++c) sum[c] += colours[i][c];
        const long count = static_cast<long>(std::max<size_t>(1, b.end - b.begin));
        out.push_back({static_cast<int>(sum[0] / count), static_cast<int>(sum[1] / count), static_cast<int>(sum[2] / count)});
    }
    return out;
}

std::array<int, 3> from15(uint16_t v) {
    const auto c = [](int x) { return x << 3 | x >> 2; };
    return {c(v & 31), c(v >> 5 & 31), c(v >> 10 & 31)};
}

}  // namespace

std::optional<Tim8> read_tim8(View tim) {
    if (tim.size() < 20 || rd32(tim, 0) != kTimMagic || rd32(tim, 4) != kTim8) return std::nullopt;
    Tim8 t;
    const size_t clut_len = rd32(tim, 8);
    t.clut_x = rd16(tim, 12);
    t.clut_y = rd16(tim, 14);
    if (rd16(tim, 16) != 256 || rd16(tim, 18) != 1 || 8 + clut_len + 12 > tim.size()) return std::nullopt;
    for (size_t i = 0; i < 256; ++i) t.palette[i] = rd16(tim, 20 + 2 * i);
    const size_t image = 8 + clut_len;
    t.x = rd16(tim, image + 4);
    t.y = rd16(tim, image + 6);
    t.w = rd16(tim, image + 8) * 2;
    t.h = rd16(tim, image + 10);
    const size_t n = static_cast<size_t>(t.w) * static_cast<size_t>(t.h);
    if (image + 12 + n > tim.size()) return std::nullopt;
    t.pixels.assign(tim.begin() + static_cast<long>(image + 12), tim.begin() + static_cast<long>(image + 12 + n));
    return t;
}

Bytes write_tim8(const Tim8& t) {
    Bytes b;
    put32(b, kTimMagic);
    put32(b, kTim8);
    put32(b, 12 + 256 * 2);
    put16(b, t.clut_x);
    put16(b, t.clut_y);
    put16(b, 256);
    put16(b, 1);
    for (const uint16_t c : t.palette) put16(b, c);
    put32(b, static_cast<uint32_t>(12 + t.pixels.size()));
    put16(b, t.x);
    put16(b, t.y);
    put16(b, static_cast<uint16_t>(t.w / 2));
    put16(b, static_cast<uint16_t>(t.h));
    b.insert(b.end(), t.pixels.begin(), t.pixels.end());
    return b;
}

Image resize(const Image& img, int w, int h) {
    Image out{w, h, {}};
    if (w <= img.w && h <= img.h && vfs::downsample_rgba(img.rgba, img.w, img.h, w, h, out.rgba)) return out;
    out.rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            std::memcpy(&out.rgba[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4],
                        px(img, x * img.w / w, y * img.h / h), 4);
    return out;
}

std::optional<Image> picture(const Image& src, int size, int tolerance) {
    if (src.w <= 0 || src.h <= 0 || src.rgba.size() != static_cast<size_t>(src.w) * static_cast<size_t>(src.h) * 4)
        return std::nullopt;
    // The background: what is like the corner and reaches an edge.
    const uint8_t* ref = px(src, 0, 0);
    const auto like_bg = [&](int x, int y) {
        const uint8_t* p = px(src, x, y);
        if (p[3] < 128) return true;
        for (int c = 0; c < 3; ++c)
            if (std::abs(p[c] - ref[c]) > tolerance) return false;
        return true;
    };
    std::vector<uint8_t> bg(static_cast<size_t>(src.w) * static_cast<size_t>(src.h), 0);
    std::deque<std::pair<int, int>> todo;
    const auto seed = [&](int x, int y) {
        const size_t i = static_cast<size_t>(y) * static_cast<size_t>(src.w) + static_cast<size_t>(x);
        if (!bg[i] && like_bg(x, y)) {
            bg[i] = 1;
            todo.emplace_back(x, y);
        }
    };
    for (int x = 0; x < src.w; ++x) {
        seed(x, 0);
        seed(x, src.h - 1);
    }
    for (int y = 0; y < src.h; ++y) {
        seed(0, y);
        seed(src.w - 1, y);
    }
    while (!todo.empty()) {
        const auto [x, y] = todo.front();
        todo.pop_front();
        if (x > 0) seed(x - 1, y);
        if (x + 1 < src.w) seed(x + 1, y);
        if (y > 0) seed(x, y - 1);
        if (y + 1 < src.h) seed(x, y + 1);
    }
    // The character's box, premultiplied (so scaling does not bleed the background's colour in).
    int x0 = src.w, y0 = src.h, x1 = -1, y1 = -1;
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x)
            if (!bg[static_cast<size_t>(y) * static_cast<size_t>(src.w) + static_cast<size_t>(x)]) {
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
            }
    if (x1 < 0) return std::nullopt;
    const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    Image crop{cw, ch, std::vector<uint8_t>(static_cast<size_t>(cw) * static_cast<size_t>(ch) * 4)};
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            const uint8_t* p = px(src, x0 + x, y0 + y);
            const bool out = bg[static_cast<size_t>(y0 + y) * static_cast<size_t>(src.w) + static_cast<size_t>(x0 + x)];
            const int a = out ? 0 : p[3];
            uint8_t* d = &crop.rgba[(static_cast<size_t>(y) * static_cast<size_t>(cw) + static_cast<size_t>(x)) * 4];
            for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>(p[c] * a / 255);
            d[3] = static_cast<uint8_t>(a);
        }
    // Fitted with a small margin, standing on the bottom edge, centred across.
    const int room = size - 2;
    const double scale = std::min(static_cast<double>(room) / cw, static_cast<double>(room) / ch);
    const int fw = std::max(1, static_cast<int>(cw * scale)), fh = std::max(1, static_cast<int>(ch * scale));
    const Image fit = resize(crop, fw, fh);
    const int ox = (size - fw) / 2, oy = size - 1 - fh;
    Image out{size, size, std::vector<uint8_t>(static_cast<size_t>(size) * static_cast<size_t>(size) * 4)};
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            std::array<int, 3> c = backdrop(x, y, size);
            const int fx = x - ox, fy = y - oy;
            if (fx >= 0 && fy >= 0 && fx < fw && fy < fh) {
                const uint8_t* p = px(fit, fx, fy);
                for (size_t k = 0; k < 3; ++k) c[k] = p[k] + c[k] * (255 - p[3]) / 255;
            }
            uint8_t* d = &out.rgba[(static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)) * 4];
            for (size_t k = 0; k < 3; ++k) d[k] = static_cast<uint8_t>(std::clamp(c[k], 0, 255));
            d[3] = 255;
        }
    return out;
}

Tim8 own_palette(const Tim8& like, const Image& img) {
    Tim8 t = like;
    t.w = img.w;
    t.h = img.h;
    std::vector<std::array<int, 3>> colours;
    for (size_t i = 0; i < img.rgba.size(); i += 4) colours.push_back({img.rgba[i], img.rgba[i + 1], img.rgba[i + 2]});
    const auto pal = median_cut(colours, 256);
    t.palette.fill(0x8000);
    for (size_t i = 0; i < pal.size(); ++i) t.palette[i] = to15(pal[i][0], pal[i][1], pal[i][2]);
    t.pixels.resize(colours.size());
    for (size_t i = 0; i < colours.size(); ++i) {
        long best = -1;
        for (size_t k = 0; k < pal.size(); ++k) {
            const auto e = from15(t.palette[k]);
            long d = 0;
            for (size_t c = 0; c < 3; ++c) d += static_cast<long>(e[c] - colours[i][c]) * (e[c] - colours[i][c]);
            if (best < 0 || d < best) {
                best = d;
                t.pixels[i] = static_cast<uint8_t>(k);
            }
        }
    }
    return t;
}

Tim8 shared_palette(const Tim8& like, const Image& img, const std::vector<bool>& keep) {
    Tim8 t = like;
    for (size_t i = 0; i < t.pixels.size() && i * 4 + 3 < img.rgba.size(); ++i) {
        if (i < keep.size() && keep[i]) continue;
        const uint8_t* p = &img.rgba[i * 4];
        t.pixels[i] = static_cast<uint8_t>(vfs::palette_index(t.palette.data(), 256, p[0], p[1], p[2], 255));
    }
    return t;
}

Consensus common_pixels(const std::vector<Tim8>& tims, int percent) {
    Consensus out;
    if (tims.size() < 2) return out;
    out.like = tims[0];
    const size_t n = tims[0].pixels.size();
    for (const Tim8& t : tims)
        if (t.pixels.size() != n) return Consensus{};
    out.keep.assign(n, false);
    for (size_t i = 0; i < n; ++i) {
        std::array<int, 256> votes{};
        for (const Tim8& t : tims) ++votes[t.pixels[i]];
        const auto top = std::max_element(votes.begin(), votes.end());
        out.like.pixels[i] = static_cast<uint8_t>(top - votes.begin());
        out.keep[i] = static_cast<long>(*top) * 100 >= static_cast<long>(tims.size()) * percent;
    }
    return out;
}

}  // namespace patch::card_art
