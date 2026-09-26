// PNG (and any stb_image-decodable) loading through the VFS. The game binary
// links the decoder only; the encoder stays tool-side (tools/asset_ripper).

#include "vfs/image.hpp"

#include "vfs/vfs.hpp"

#define STB_IMAGE_IMPLEMENTATION
// No 64-bit file API needed: everything arrives as a memory buffer.
#define STBI_NO_STDIO
#include "stb/stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vfs {

bool decode_rgba(const uint8_t* data, size_t size, int& w, int& h, std::vector<uint8_t>& rgba) {
    w = h = 0;
    rgba.clear();
    if (!data || !size || size > (1u << 31)) return false;
    int channels = 0;
    // 4 forces RGBA output regardless of source channels.
    stbi_uc* pixels =
        stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels, 4);
    if (!pixels || w <= 0 || h <= 0) {
        stbi_image_free(pixels);
        return false;
    }
    const size_t total = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    rgba.assign(pixels, pixels + total);
    stbi_image_free(pixels);
    return true;
}

bool load_rgba(const Vfs& vfs, const std::string& name, int& w, int& h, std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> blob;
    if (!vfs.read(name, blob)) return false;
    return decode_rgba(blob.data(), blob.size(), w, h, rgba);
}

bool downsample_rgba(const std::vector<uint8_t>& src, int src_w, int src_h, int dst_w, int dst_h,
                     std::vector<uint8_t>& dst) {
    dst.clear();
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return false;
    if (src.size() != static_cast<size_t>(src_w) * static_cast<size_t>(src_h) * 4) return false;
    if (dst_w > src_w || dst_h > src_h) return false;  // never invent detail by upsampling
    dst.resize(static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h) * 4);
    if (dst_w == src_w && dst_h == src_h) {
        dst = src;
        return true;
    }
    // Box filter: each destination pixel averages the source box covering it.
    for (int y = 0; y < dst_h; ++y) {
        const int y0 = y * src_h / dst_h, y1 = (y + 1) * src_h / dst_h;
        for (int x = 0; x < dst_w; ++x) {
            const int x0 = x * src_w / dst_w, x1 = (x + 1) * src_w / dst_w;
            uint32_t acc[4] = {};
            uint32_t n = 0;
            for (int sy = y0; sy < y1; ++sy) {
                for (int sx = x0; sx < x1; ++sx) {
                    const uint8_t* p = &src[(static_cast<size_t>(sy) * static_cast<size_t>(src_w) +
                                             static_cast<size_t>(sx)) *
                                            4];
                    for (int c = 0; c < 4; ++c) acc[c] += p[c];
                    ++n;
                }
            }
            uint8_t* d = &dst[(static_cast<size_t>(y) * static_cast<size_t>(dst_w) + static_cast<size_t>(x)) * 4];
            for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>((acc[c] + n / 2) / (n ? n : 1));
        }
    }
    return true;
}

}  // namespace vfs
