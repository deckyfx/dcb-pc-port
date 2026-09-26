#pragma once
// PNG (and any stb_image-decodable) loading through the VFS, for HD replacement
// textures. Encoding (stb_image_write) is intentionally tool-only — see
// tools/asset_ripper — so the game binary never pays for the encoder.

#include <cstdint>
#include <string>
#include <vector>

namespace vfs {

class Vfs;

/// Decode any stb-supported image to RGBA8888. Returns false on corrupt/unknown data.
bool decode_rgba(const uint8_t* data, size_t size, int& w, int& h, std::vector<uint8_t>& rgba);

/// Load `name` from the VFS and decode it to RGBA8888.
bool load_rgba(const Vfs& vfs, const std::string& name, int& w, int& h, std::vector<uint8_t>& rgba);

/// Box-downsample RGBA `src_w`x`src_h` to `dst_w`x`dst_h` (both > 0). Used when an
/// upscaled PNG is larger than the VRAM rect it replaces; 1:1 art passes through
/// a plain copy. Upsampling is refused (returns false) — ship 1:1-or-larger packs.
bool downsample_rgba(const std::vector<uint8_t>& src, int src_w, int src_h, int dst_w, int dst_h,
                     std::vector<uint8_t>& dst);

}  // namespace vfs
