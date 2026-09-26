#include "mdec/mdec.hpp"

#include <algorithm>

namespace hle {

namespace {

/// Natural (row-major) index of each zigzag position: psx-spx "MDEC Decompression" builds this as
/// zagzig[zigzag[i]] = i from its zigzag table.
constexpr std::array<uint8_t, 64> kZagzig = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/// Status bits 18-16 for each colour block in decode order (psx-spx: 0..3=Y1..Y4, 4=Cr, 5=Cb).
constexpr std::array<uint32_t, 6> kStatusBlock = {4, 5, 0, 1, 2, 3};

constexpr uint16_t kEndOfBlock = 0xFE00;

int32_t sext10(uint32_t v) { return static_cast<int32_t>((v & 0x3FFu) ^ 0x200u) - 0x200; }
int32_t sext9(int32_t v) { return static_cast<int32_t>((static_cast<uint32_t>(v) & 0x1FFu) ^ 0x100u) - 0x100; }

/// Separable 8x8 IDCT with the uploaded scale table (entries are 2^16 * C(u)/2 * cos(...), so the
/// two passes scale by 2^32). The result is rounded, wrapped to 9 bits and saturated to -128..127
/// like the hardware; psx-spx "real_idct_core" (DuckStation's hardware-verified variant of it).
void idct(std::array<int16_t, 64>& blk, const std::array<int16_t, 64>& scale) {
    std::array<int32_t, 64> tmp{};  // |blk| <= 0x400, |scale| <= 0x8000: 8 terms fit in 31 bits
    for (unsigned x = 0; x < 8; ++x) {
        // Most columns of a quantised block are all zero; their contribution is zero too.
        bool any = false;
        for (unsigned u = 0; u < 8; ++u) any |= blk[u * 8 + x] != 0;
        if (!any) continue;
        for (unsigned y = 0; y < 8; ++y) {
            int32_t sum = 0;
            for (unsigned u = 0; u < 8; ++u) sum += int32_t{blk[u * 8 + x]} * scale[u * 8 + y];
            tmp[x + y * 8] = sum;
        }
    }
    for (unsigned y = 0; y < 8; ++y) {
        for (unsigned x = 0; x < 8; ++x) {
            int64_t sum = 0;
            for (unsigned u = 0; u < 8; ++u) sum += int64_t{tmp[u + y * 8]} * scale[u * 8 + x];
            const auto rounded = static_cast<int32_t>((sum >> 32) + ((sum >> 31) & 1));
            blk[x + y * 8] = static_cast<int16_t>(std::clamp(sext9(rounded), -128, 127));
        }
    }
}

/// 8-bit component to the value the MDEC outputs: unsigned output adds 128 (psx-spx: "xor 80h").
uint32_t out8(int32_t v, bool is_signed) { return (static_cast<uint32_t>(v) & 0xFFu) ^ (is_signed ? 0u : 0x80u); }

/// 8-bit component (already in output form) to 5 bits, rounded with saturation (Mednafen's
/// conversion). Rounding happens in the unsigned domain; signed output flips the top bit back.
uint32_t to5(uint32_t c, bool is_signed) {
    const uint32_t u = is_signed ? c ^ 0x80u : c;
    const uint32_t r = std::min((u + 4u) >> 3, 0x1Fu);
    return is_signed ? r ^ 0x10u : r;
}

}  // namespace

void Mdec::reset() {
    state_ = State::Idle;
    command_ = 0;
    remaining_ = 0;
    param_field_ = 0;
    table_pos_ = 0;
    coef_ = 64;
    q_scale_ = 0;
    block_ = 0;
    out_.clear();
    out_pos_ = 0;
}

void Mdec::write_control(uint32_t word) {
    if (word & 0x80000000u) reset();
    dma_in_enabled_ = (word >> 30) & 1u;
    dma_out_enabled_ = (word >> 29) & 1u;
}

uint32_t Mdec::status() const {
    uint32_t s = 0;
    if (output_words_available() == 0) s |= 1u << 31;  // data-out FIFO empty
    // Bit 30 (data-in FIFO full) never sets: input is consumed as it arrives.
    if (state_ != State::Idle) s |= 1u << 29;              // busy receiving parameters
    if (dma_in_enabled_) s |= 1u << 28;                    // input always has room
    if (dma_out_enabled_ && output_words_available() != 0) s |= 1u << 27;
    s |= ((command_ >> 25) & 0xFu) << 23;  // command bits 25-28 -> depth/signed/bit15 (26-23)
    s |= (colour() ? kStatusBlock[block_] : 4u) << 16;
    return s | param_field_;
}

uint32_t Mdec::read_data() {
    if (out_pos_ >= out_.size()) return 0;
    const uint32_t w = out_[out_pos_++];
    if (out_pos_ == out_.size()) {  // drained: reuse the buffer from the start
        out_.clear();
        out_pos_ = 0;
    }
    return w;
}

void Mdec::dma_write(const uint32_t* words, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) write_command(words[i]);
}

void Mdec::dma_read(uint32_t* words, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) words[i] = read_data();
}

void Mdec::write_command(uint32_t word) {
    if (state_ == State::Idle) {
        start_command(word);
        return;
    }
    parameter(word);
    --remaining_;
    param_field_ = static_cast<uint16_t>(remaining_ - 1);  // 0xFFFF once complete
    if (remaining_ == 0) {
        state_ = State::Idle;
        coef_ = 64;  // a block cut short by the parameter count is dropped
        block_ = 0;
    }
}

void Mdec::start_command(uint32_t word) {
    command_ = word;
    table_pos_ = 0;
    switch (word >> 29) {
    case 1:  // MDEC(1): decode macroblocks; bits 15-0 = parameter word count
        remaining_ = word & 0xFFFFu;
        state_ = State::Decode;
        coef_ = 64;
        block_ = 0;
        break;
    case 2:  // MDEC(2): quant tables; bit 0 = luminance and colour (else luminance only)
        remaining_ = (word & 1u) ? 32u : 16u;
        state_ = State::QuantTable;
        break;
    case 3:  // MDEC(3): IDCT scale table, 64 signed halfwords
        remaining_ = 32;
        state_ = State::ScaleTable;
        break;
    default:
        // MDEC(0)/(4-7): no function; bits 15-0 are mirrored to the status without the
        // "minus 1" and no parameters follow (psx-spx "MDEC(0) - No function").
        param_field_ = static_cast<uint16_t>(word);
        return;
    }
    param_field_ = static_cast<uint16_t>(remaining_ - 1);
    if (remaining_ == 0) state_ = State::Idle;
}

void Mdec::parameter(uint32_t word) {
    switch (state_) {
    case State::Decode:
        decode_halfword(static_cast<uint16_t>(word));
        decode_halfword(static_cast<uint16_t>(word >> 16));
        break;
    case State::QuantTable:
        // 64 unsigned bytes per table, in zigzag order (psx-spx "MDEC(2) - Set Quant Table(s)").
        for (unsigned b = 0; b < 4; ++b) {
            const unsigned i = table_pos_ * 4 + b;
            auto& table = i < 64 ? quant_y_ : quant_uv_;
            table[i % 64] = static_cast<uint8_t>(word >> (8 * b));
        }
        ++table_pos_;
        break;
    case State::ScaleTable:
        scale_[table_pos_ * 2] = static_cast<int16_t>(word);
        scale_[table_pos_ * 2 + 1] = static_cast<int16_t>(word >> 16);
        ++table_pos_;
        break;
    case State::Idle:
        break;
    }
}

/// One halfword of run-level data (psx-spx "MDEC Decompression", rl_decode_block). The first
/// halfword of a block is the DC value plus a 6-bit quant scale; FE00h there is padding. Each
/// following halfword is a 6-bit zero run and a 10-bit AC value; the block ends at FE00h (whose
/// run pushes past 63) or once coefficient 63 has been written.
void Mdec::decode_halfword(uint16_t n) {
    const auto& qt = (colour() && block_ < 2) ? quant_uv_ : quant_y_;
    int32_t val = 0;
    if (coef_ == 64) {
        if (n == kEndOfBlock) return;
        blk_.fill(0);
        coef_ = 0;
        q_scale_ = (n >> 10) & 0x3Fu;
        val = q_scale_ ? sext10(n) * qt[0] : sext10(n) * 2;
    } else {
        coef_ += ((n >> 10) & 0x3Fu) + 1;
        if (coef_ < 64) {
            val = q_scale_ ? (sext10(n) * qt[coef_] * static_cast<int32_t>(q_scale_) + 4) / 8
                           : sext10(n) * 2;
        }
    }
    if (coef_ < 64) {
        val = std::clamp(val, -0x400, 0x3FF);
        // q_scale 0 stores the coefficients unscaled and in natural order.
        blk_[q_scale_ ? kZagzig[coef_] : coef_] = static_cast<int16_t>(val);
    }
    if (coef_ >= 63) finish_block();
}

void Mdec::finish_block() {
    coef_ = 64;
    idct(blk_, scale_);
    if (!colour()) {
        emit_mono();
        return;
    }
    // Decode order Cr, Cb, Y1 (top-left), Y2 (top-right), Y3 (bottom-left), Y4 (bottom-right).
    if (block_ == 0) {
        cr_ = blk_;
    } else if (block_ == 1) {
        cb_ = blk_;
    } else {
        const unsigned y = block_ - 2;
        yuv_to_rgb((y & 1u) * 8, (y >> 1) * 8);
    }
    if (++block_ == 6) {
        block_ = 0;
        emit_colour();
    }
}

/// psx-spx "yuv_to_rgb": chroma is 8x8 for the whole 16x16 macroblock, so each chroma sample
/// covers 2x2 pixels. The products truncate toward zero like the reference's float math.
void Mdec::yuv_to_rgb(unsigned xx, unsigned yy) {
    const bool sgn = is_signed();
    for (unsigned y = 0; y < 8; ++y) {
        for (unsigned x = 0; x < 8; ++x) {
            const unsigned c = (x + xx) / 2 + ((y + yy) / 2) * 8;
            const int32_t cr = cr_[c], cb = cb_[c];
            const int32_t g = (-3437 * cb - 7143 * cr) / 10000;  // -0.3437*B - 0.7143*R
            const int32_t r = (1402 * cr) / 1000;                // 1.402*R
            const int32_t b = (1772 * cb) / 1000;                // 1.772*B
            const int32_t lum = blk_[x + y * 8];
            rgb_[(x + xx) + (y + yy) * 16] = out8(std::clamp(lum + r, -128, 127), sgn) |
                                             out8(std::clamp(lum + g, -128, 127), sgn) << 8 |
                                             out8(std::clamp(lum + b, -128, 127), sgn) << 16;
        }
    }
}

/// 16x16 pixels row by row (psx-spx "MDEC Data Format"): 24-bit packs R,G,B bytes back to back
/// (3 bytes per pixel, 192 words); 15-bit is BGR555 (+bit15), two pixels per word, low first.
void Mdec::emit_colour() {
    const bool sgn = is_signed();
    if (depth() == 2) {
        uint32_t acc = 0;
        unsigned bytes = 0;
        for (const uint32_t px : rgb_) {
            for (unsigned i = 0; i < 3; ++i) {
                acc |= ((px >> (8 * i)) & 0xFFu) << (8 * bytes);
                if (++bytes == 4) {
                    out_.push_back(acc);
                    acc = 0;
                    bytes = 0;
                }
            }
        }
        return;
    }
    const uint32_t a = set_bit15() ? 0x8000u : 0u;
    for (size_t i = 0; i < rgb_.size(); i += 2) {
        uint32_t w = 0;
        for (size_t k = 0; k < 2; ++k) {
            const uint32_t px = rgb_[i + k];
            const uint32_t p15 = to5(px & 0xFFu, sgn) | to5((px >> 8) & 0xFFu, sgn) << 5 |
                                 to5((px >> 16) & 0xFFu, sgn) << 10 | a;
            w |= p15 << (16 * k);
        }
        out_.push_back(w);
    }
}

/// 8x8 luminance row by row: 8-bit is 4 pixels per word (16 words), 4-bit keeps the top nibble
/// of each pixel, 8 per word, low nibble first (8 words). psx-spx "y_to_mono".
void Mdec::emit_mono() {
    const bool sgn = is_signed();
    if (depth() == 1) {
        for (unsigned i = 0; i < 64; i += 4) {
            uint32_t w = 0;
            for (unsigned k = 0; k < 4; ++k) w |= out8(blk_[i + k], sgn) << (8 * k);
            out_.push_back(w);
        }
        return;
    }
    for (unsigned i = 0; i < 64; i += 8) {
        uint32_t w = 0;
        for (unsigned k = 0; k < 8; ++k) w |= (out8(blk_[i + k], sgn) >> 4) << (4 * k);
        out_.push_back(w);
    }
}

// ---------------------------------------------------------------------------------------------
// Save state

void Mdec::save_state(psx::StateWriter& w) const {
    w.begin(psx::state_tag("MDEC"), 1);
    w.pod(state_);
    w.u32(command_);
    w.u32(remaining_);
    w.u16(param_field_);
    w.u32(table_pos_);
    w.boolean(dma_in_enabled_);
    w.boolean(dma_out_enabled_);
    w.pod(quant_y_);
    w.pod(quant_uv_);
    w.pod(scale_);
    w.pod(blk_);
    w.u32(coef_);
    w.u32(q_scale_);
    w.u32(block_);
    w.pod(cr_);
    w.pod(cb_);
    w.pod(rgb_);
    w.vec(out_);
    w.size(out_pos_);
    w.end();
}

void Mdec::load_state(psx::StateReader& r) {
    r.begin(psx::state_tag("MDEC"), 1);
    r.pod(state_);
    if (state_ > State::ScaleTable) r.fail("bad MDEC state");
    command_ = r.u32();
    remaining_ = r.u32();
    param_field_ = r.u16();
    table_pos_ = r.u32();
    dma_in_enabled_ = r.boolean();
    dma_out_enabled_ = r.boolean();
    r.pod(quant_y_);
    r.pod(quant_uv_);
    r.pod(scale_);
    r.pod(blk_);
    coef_ = r.u32();
    q_scale_ = r.u32();
    block_ = r.u32();
    r.pod(cr_);
    r.pod(cb_);
    r.pod(rgb_);
    r.vec(out_, size_t{1} << 24);
    out_pos_ = r.size(out_.size(), 0);
    r.end();
}

}  // namespace hle
