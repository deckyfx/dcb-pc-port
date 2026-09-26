#pragma once
// PlayStation GPU: GP0 drawing/transfer commands, GP1 display control, GPUSTAT/GPUREAD, and a
// software rasterizer that draws straight into a 1024x512 16-bit VRAM. Every command completes
// the moment its last word arrives, so the GPU is never busy.
// Reference: psx-spx "Graphics Processing Unit (GPU)" and its sub-chapters (GPU I/O Ports, DMA
// and Command/Status Registers; GPU Render Polygon/Line/Rectangle Commands; GPU Rendering
// Attributes; GPU Memory Transfer Commands; GPU Other Commands; GPU Display Control Commands).

#include <psx/state.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace hle {

class HdTextures;

/// Software model of the PS1 GPU (CXD8561 "new" GPU, GP1(10h) version 2).
class Gpu {
public:
    static constexpr int kVramWidth = 1024, kVramHeight = 512;  // 16-bit pixels

    Gpu();
    ~Gpu();  // defined in gpu.cpp where HdTextures is complete (unique_ptr member)

    /// GP0 port 0x1F801810 write: commands + their parameter/data words.
    void gp0(uint32_t word);
    /// Feed several GP0 words (DMA channel 2 block / linked-list payloads).
    void gp0_block(const uint32_t* words, std::size_t count);
    /// GP1 port 0x1F801814 write: control commands.
    void gp1(uint32_t word);
    /// 0x1F801810 read: VRAM->CPU transfer data, or the latched GP1(10h) info reply.
    uint32_t gpuread();
    /// 0x1F801814 read: full GPUSTAT. Ready bits 26/27/28 are always set.
    uint32_t gpustat() const;

    /// 1024x512 VRAM, row-major, PS1 pixel format (bit15 mask, 5:5:5 BGR, red in bits 0-4).
    const uint16_t* vram() const { return vram_.data(); }

    /// Display configuration from GP1(03h,05h,06h,07h,08h).
    struct Display {
        int x = 0, y = 0;               ///< display area start in VRAM
        int width = 320, height = 240;  ///< derived from GP1(08h) horizontal/vertical resolution
        bool rgb24 = false, enabled = false, interlaced = false;
    };
    Display display() const;

    /// Call once per emulated vblank: flips the interlace field (GPUSTAT bits 13/31).
    void vblank();
    /// GP0(1Fh) sets the GPU IRQ flag (GPUSTAT bit 24); the owner raises interrupt line 1 when it
    /// goes high, and GP1(02h) clears it.
    bool irq_pending() const { return irq_; }
    /// GP1(04h) DMA direction: 0 off, 1 FIFO, 2 CPU->GP0, 3 GPUREAD->CPU.
    uint32_t dma_direction() const { return dma_dir_; }
    /// True while a GP0(A0h) upload is consuming data words.
    bool receiving_vram() const { return mode_ == Mode::CpuToVram; }
    /// True while a GP0(C0h) download still has data to hand out through gpuread().
    bool sending_vram() const { return read_.remaining != 0; }

    /// Save state: VRAM, command assembly, drawing environment and display control ("GPU ").
    void save_state(psx::StateWriter& w) const;
    void load_state(psx::StateReader& r);

    /// HD texture replacement (nullptr until install_hd() is called by the owner).
    /// When installed and enabled, staged GP0(A0h) uploads are hashed and may be
    /// substituted with upscaled art; otherwise uploads commit verbatim.
    HdTextures* install_hd();
    HdTextures* hd() { return hd_.get(); }
    const HdTextures* hd() const { return hd_.get(); }

    /// Save-state support for a mid-upload GP0(A0h) transfer. A VBLANK yield can
    /// land between data words (generated loops poll on back-edges), so a save
    /// taken there must capture the staged words, the destination cursor and
    /// the HD flag — otherwise the load commits a half/garbled upload. The
    /// HdTextures runtime snapshot (CLUT sniffer + caches) is separate; see
    /// HdTextures::save()/load_snapshot(). fifo_/mode_/write_ are covered by
    /// the GPU's own snapshot alongside these members.
    struct UploadSnapshot {
        int32_t x = 0, y = 0, w = 0, h = 0;  ///< write_ destination rect
        int32_t cx = 0, cy = 0;              ///< write_ progress cursor
        uint32_t remaining = 0;              ///< write_ pixels left
        std::vector<uint32_t> staged;        ///< staged GP0(A0h) words
        bool hd_staging = false;
        bool active = false;  ///< true while mode_ == Mode::CpuToVram
    };
    UploadSnapshot save_upload() const;
    void load_upload(const UploadSnapshot& snap);

private:
    /// A vertex after draw-offset application, with 8-bit color and texture coordinates.
    struct Vertex {
        int32_t x = 0, y = 0;
        int32_t r = 0, g = 0, b = 0;
        int32_t u = 0, v = 0;
    };
    /// Per-primitive rendering state resolved once before rasterizing.
    struct Prim {
        bool textured = false, raw = false, semi = false, gouraud = false, dither = false;
        uint32_t semi_mode = 0;         ///< 0: B/2+F/2, 1: B+F, 2: B-F, 3: B+F/4
        uint32_t depth = 0;             ///< 0: 4-bit CLUT, 1: 8-bit CLUT, 2/3: 15-bit direct
        int32_t tex_x = 0, tex_y = 0;   ///< texture page base in VRAM
        int32_t clut_x = 0, clut_y = 0; ///< CLUT position in VRAM
    };
    /// Active GP0(A0h)/GP0(C0h) rectangle transfer.
    struct Transfer {
        int32_t x = 0, y = 0, w = 0, h = 0;
        int32_t cx = 0, cy = 0;  ///< progress within the rectangle
        uint32_t remaining = 0;  ///< pixels left
    };
    enum class Mode { Command, CpuToVram, Polyline };

    std::vector<uint16_t> vram_;
    std::unique_ptr<HdTextures> hd_;  ///< null until install_hd()
    std::vector<uint32_t> staged_;    ///< staged GP0(A0h) words while HD is armed
    bool hd_staging_ = false;

    // GP0 command assembly
    Mode mode_ = Mode::Command;
    std::array<uint32_t, 16> fifo_{};
    std::size_t fifo_len_ = 0, fifo_need_ = 0;
    Transfer write_{}, read_{};
    // Polyline continuation state (GP0 48h-5Fh)
    Vertex poly_last_{};
    uint32_t poly_color_ = 0;
    bool poly_gouraud_ = false, poly_have_color_ = false;
    Prim poly_prim_{};

    // Drawing environment (GP0 E1h-E6h)
    uint32_t draw_mode_ = 0;     ///< E1h bits 0-13
    uint32_t tex_window_ = 0;    ///< E2h bits 0-19
    uint32_t area_tl_ = 0, area_br_ = 0;  ///< E3h/E4h bits 0-19
    uint32_t offset_raw_ = 0;    ///< E5h bits 0-21
    int32_t area_x1_ = 0, area_y1_ = 0, area_x2_ = 0, area_y2_ = 0;
    int32_t offset_x_ = 0, offset_y_ = 0;
    uint32_t tw_and_x_ = 0xFF, tw_or_x_ = 0, tw_and_y_ = 0xFF, tw_or_y_ = 0;
    bool set_mask_ = false, check_mask_ = false;

    // Display / control (GP1)
    uint32_t display_mode_ = 0;  ///< GP1(08h) bits 0-7
    uint32_t display_start_ = 0, hrange_ = 0, vrange_ = 0;
    uint32_t dma_dir_ = 0;
    bool display_disabled_ = true, irq_ = false, allow_tex_disable_ = false, field_ = false;
    uint32_t gpuread_latch_ = 0;

    void reset();
    void reset_command_buffer();
    static std::size_t command_length(uint32_t cmd);
    void execute();
    void environment(uint32_t word);

    // Primitive setup
    void draw_polygon();
    void draw_line_cmd();
    void polyline_word(uint32_t word);
    void draw_rect();
    void fill_rect();
    void copy_vram();
    void begin_cpu_to_vram();
    void begin_vram_to_cpu();
    void write_transfer_word(uint32_t word);
    void write_transfer_word_direct(uint32_t word);  ///< masked VRAM write, HD bypass
    void commit_staged_upload();  ///< hash staged_ via HdTextures, commit winner to VRAM

    Prim make_prim(uint32_t cmd, bool textured, uint32_t texpage, uint32_t clut) const;
    Vertex decode_vertex(uint32_t xy, uint32_t color) const;

    // Rasterizers
    void draw_triangle(const Vertex& v0, const Vertex& v1, const Vertex& v2, const Prim& p);
    void draw_line(const Vertex& a, const Vertex& b, const Prim& p);
    void draw_sprite(const Vertex& origin, int32_t w, int32_t h, const Prim& p);

    // Pixel pipeline
    uint16_t fetch_texel(const Prim& p, int32_t u, int32_t v) const;
    void plot(int32_t x, int32_t y, int32_t r, int32_t g, int32_t b, int32_t u, int32_t v, const Prim& p);
    void put_masked(int32_t x, int32_t y, uint16_t pixel);

    uint16_t& at(int32_t x, int32_t y) { return vram_[static_cast<std::size_t>((y & 511) * kVramWidth + (x & 1023))]; }
    uint16_t at(int32_t x, int32_t y) const {
        return vram_[static_cast<std::size_t>((y & 511) * kVramWidth + (x & 1023))];
    }
};

}  // namespace hle
